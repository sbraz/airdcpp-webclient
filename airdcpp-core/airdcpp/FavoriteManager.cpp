/* 
 * Copyright (C) 2001-2015 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdinc.h"
#include "FavoriteManager.h"

#include "AirUtil.h"
#include "BZUtils.h"
#include "ClientManager.h"
#include "CryptoManager.h"
#include "FilteredFile.h"
#include "HttpConnection.h"
#include "LogManager.h"
#include "RelevancySearch.h"
#include "ResourceManager.h"
#include "ShareManager.h"
#include "SimpleXML.h"
#include "StringTokenizer.h"
#include "UserCommand.h"

namespace dcpp {

#define CONFIG_FAV_NAME "Favorites.xml"
#define CONFIG_RECENTS_NAME "Recents.xml"
#define CONFIG_DIR Util::PATH_USER_CONFIG

using boost::range::for_each;
using boost::range::find_if;

FavoriteManager::FavoriteManager() : lastId(0), useHttp(false), running(false), c(nullptr), lastServer(0), listType(TYPE_NORMAL), dontSave(false) {
	SettingsManager::getInstance()->addListener(this);
	ClientManager::getInstance()->addListener(this);
	ShareManager::getInstance()->addListener(this);

	File::ensureDirectory(Util::getHubListsPath());
}

FavoriteManager::~FavoriteManager() {
	ClientManager::getInstance()->removeListener(this);
	SettingsManager::getInstance()->removeListener(this);
	ShareManager::getInstance()->removeListener(this);

	if(c) {
		c->removeListener(this);
		delete c;
		c = nullptr;
	}

	for_each(previewApplications, DeleteFunction());
}

UserCommand FavoriteManager::addUserCommand(int type, int ctx, Flags::MaskType flags, const string& name, const string& command, const string& to, const string& hub) {
	
	// The following management is to protect users against malicious hubs or clients.
	// Hubs (or clients) can send an arbitrary amount of user commands, which means that there is a possibility that
	// the client will need to manage thousands and thousands of user commands.
	// This can naturally cause problems with memory etc, so the client may even crash at some point.
	// The following management tries to remedy this problem by doing two things;
	// a) Replaces previous user commands (if they have the same name etc)
	// b) Restricts the amount of user commands that pertain to a particlar hub
	// Note that this management only cares about externally created user commands, 
	// which means that the user themselves can create however large user commands.
	if (flags == UserCommand::FLAG_NOSAVE)
	{
		const int maximumUCs = 2000; // Completely arbitrary
		int externalCommands = 0; // Used to count the number of external commands
		RLock l(cs);
		for (auto& uc : userCommands) {
			if ((uc.isSet(UserCommand::FLAG_NOSAVE)) &&	// Only care about external commands...
				(uc.getHub() == hub))	// ... and those pertaining to this particular hub.
			{
				++externalCommands;

				// If the UC is generally identical otherwise, change the command
				if ((uc.getName() == name) &&
					(uc.getCtx() == ctx) &&
					(uc.getType() == type) &&
					(uc.isSet(flags)) &&
					(uc.getTo() == to))
				{
					uc.setCommand(command);
					return uc;
				}
			}

		}

		// Validate if there's too many user commands
		if (maximumUCs <= externalCommands)
		{
			return userCommands.back();
		}
	}
	
	// No dupes, add it...
	auto cmd = UserCommand(lastId++, type, ctx, flags, name, command, to, hub);

	{
		WLock l(cs);
		userCommands.emplace_back(cmd);
	}

	if(!cmd.isSet(UserCommand::FLAG_NOSAVE)) 
		save();

	return cmd;
}

bool FavoriteManager::getUserCommand(int cid, UserCommand& uc) {
	WLock l(cs);
	for(auto& u: userCommands) {
		if(u.getId() == cid) {
			uc = u;
			return true;
		}
	}
	return false;
}

bool FavoriteManager::moveUserCommand(int cid, int pos) {
	dcassert(pos == -1 || pos == 1);
	WLock l(cs);
	for(auto i = userCommands.begin(); i != userCommands.end(); ++i) {
		if(i->getId() == cid) {
			swap(*i, *(i + pos));
			return true;
		}
	}
	return false;
}

void FavoriteManager::updateUserCommand(const UserCommand& uc) {
	bool nosave = true;
	{
		WLock l(cs);
		for(auto i = userCommands.begin(); i != userCommands.end(); ++i) {
			if(i->getId() == uc.getId()) {
				*i = uc;
				nosave = uc.isSet(UserCommand::FLAG_NOSAVE);
				break;
			}
		}
	}

	if(!nosave)
		save();
}

int FavoriteManager::findUserCommand(const string& aName, const string& aUrl) {
	RLock l(cs);
	for(auto i = userCommands.begin(); i != userCommands.end(); ++i) {
		if(i->getName() == aName && i->getHub() == aUrl) {
			return i->getId();
		}
	}
	return -1;
}

void FavoriteManager::removeUserCommand(int cid) {
	bool nosave = true;
	{
		WLock l(cs);
		for(auto i = userCommands.begin(); i != userCommands.end(); ++i) {
			if(i->getId() == cid) {
				nosave = i->isSet(UserCommand::FLAG_NOSAVE);
				userCommands.erase(i);
				break;
			}
		}
	}

	if(!nosave)
		save();
}
void FavoriteManager::removeUserCommand(const string& srv) {
	WLock l(cs);
	userCommands.erase(std::remove_if(userCommands.begin(), userCommands.end(), [&](const UserCommand& uc) {
		return uc.getHub() == srv && uc.isSet(UserCommand::FLAG_NOSAVE);
	}), userCommands.end());

}

void FavoriteManager::removeHubUserCommands(int ctx, const string& hub) {
	WLock l(cs);
	for(auto i = userCommands.begin(); i != userCommands.end(); ) {
		if(i->getHub() == hub && i->isSet(UserCommand::FLAG_NOSAVE) && i->getCtx() & ctx) {
			i = userCommands.erase(i);
		} else {
			++i;
		}
	}
}

void FavoriteManager::addFavoriteUser(const HintedUser& aUser) {
	if(aUser.user == ClientManager::getInstance()->getMe()) // we cant allow adding ourself as a favorite user :P
		return;

	{
		RLock l(cs);
		if(users.find(aUser.user->getCID()) != users.end()) {
			return;
		}
	}

	string nick;

	//prefer to use the add nick
	ClientManager* cm = ClientManager::getInstance();
	{
		RLock l(cm->getCS());
		auto ou = cm->findOnlineUser(aUser.user->getCID(), aUser.hint);
		if (!ou) {
			auto nicks = move(ClientManager::getInstance()->getNicks(aUser.user->getCID(), false));
			if (!nicks.empty())
				nick = nicks[0];
		} else {
			nick = ou->getIdentity().getNick();
		}
	}

	auto fu = FavoriteUser(aUser, nick, aUser.hint, aUser.user->getCID().toBase32());
	{
		WLock l (cs);
		users.emplace(aUser.user->getCID(), fu);
	}

	aUser.user->setFlag(User::FAVORITE);
	fire(FavoriteManagerListener::UserAdded(), fu);
}

void FavoriteManager::removeFavoriteUser(const UserPtr& aUser) {
	{
		WLock l(cs);
		auto i = users.find(aUser->getCID());
		if(i != users.end()) {
			aUser->unsetFlag(User::FAVORITE);
			fire(FavoriteManagerListener::UserRemoved(), i->second);
			users.erase(i);
		}
	}

	save();
}

optional<FavoriteUser> FavoriteManager::getFavoriteUser(const UserPtr &aUser) const {
	RLock l(cs);
	auto i = users.find(aUser->getCID());
	return i == users.end() ? optional<FavoriteUser>() : i->second;
}


void FavoriteManager::changeLimiterOverride(const UserPtr& aUser) noexcept{
	RLock l(cs);
	auto i = users.find(aUser->getCID());
	if (i != users.end()) {
		if (!i->second.isSet(FavoriteUser::FLAG_SUPERUSER))
			i->second.setFlag(FavoriteUser::FLAG_SUPERUSER);
		else
			i->second.unsetFlag(FavoriteUser::FLAG_SUPERUSER);
	}
}

bool FavoriteManager::addFavoriteDir(const string& aName, StringList& aTargets){
	auto p = find_if(favoriteDirs, CompareFirst<string, StringList>(aName));
	if (p != favoriteDirs.end())
		return false;

	sort(aTargets.begin(), aTargets.end());
	favoriteDirs.emplace_back(aName, aTargets);
	save();
	return true;
}

void FavoriteManager::saveFavoriteDirs(FavDirList& dirs) {
	favoriteDirs.clear();
	favoriteDirs = dirs;
	save();
}

HubEntryList FavoriteManager::getPublicHubs() {
	RLock l(cs);
	return publicListMatrix[publicListServer];
}

void FavoriteManager::removeallRecent() {
	{
		WLock l(cs);
		recentHubs.clear();
	}

	recentsave();
}


void FavoriteManager::addRecent(const RecentHubEntryPtr& aEntry) {
	{
		WLock l(cs);
		auto i = getRecentHub(aEntry->getServer());
		if (i != recentHubs.end()) {
			return;
		}

		recentHubs.push_back(aEntry);
	}

	fire(FavoriteManagerListener::RecentAdded(), aEntry);
	recentsave();
}

void FavoriteManager::removeRecent(const RecentHubEntryPtr& entry) {
	{
		WLock l(cs);
		auto i = find(recentHubs.begin(), recentHubs.end(), entry);
		if (i == recentHubs.end()) {
			return;
		}

		fire(FavoriteManagerListener::RecentRemoved(), entry);
		recentHubs.erase(i);
	}

	recentsave();
}

void FavoriteManager::updateRecent(const RecentHubEntryPtr& entry) {
	{
		RLock l(cs);
		auto i = find(recentHubs.begin(), recentHubs.end(), entry);
		if (i == recentHubs.end()) {
			return;
		}
	}
		
	fire(FavoriteManagerListener::RecentUpdated(), entry);
	recentsave();
}

class XmlListLoader : public SimpleXMLReader::CallBack {
public:
	XmlListLoader(HubEntryList& lst) : publicHubs(lst) { }
	~XmlListLoader() { }
	void startTag(const string& aName, StringPairList& attribs, bool) {
		if(aName == "Hub") {
			const string& name = getAttrib(attribs, "Name", 0);
			const string& server = getAttrib(attribs, "Address", 1);
			const string& description = getAttrib(attribs, "Description", 2);
			const string& users = getAttrib(attribs, "Users", 3);
			const string& country = getAttrib(attribs, "Country", 4);
			const string& shared = getAttrib(attribs, "Shared", 5);
			const string& minShare = getAttrib(attribs, "Minshare", 5);
			const string& minSlots = getAttrib(attribs, "Minslots", 5);
			const string& maxHubs = getAttrib(attribs, "Maxhubs", 5);
			const string& maxUsers = getAttrib(attribs, "Maxusers", 5);
			const string& reliability = getAttrib(attribs, "Reliability", 5);
			const string& rating = getAttrib(attribs, "Rating", 5);
			publicHubs.push_back(HubEntry(name, server, description, users, country, shared, minShare, minSlots, maxHubs, maxUsers, reliability, rating));
		}
	}
private:
	HubEntryList& publicHubs;
};

bool FavoriteManager::onHttpFinished(bool fromHttp) noexcept {
	MemoryInputStream mis(downloadBuf);
	bool success = true;

	WLock l(cs);
	HubEntryList& list = publicListMatrix[publicListServer];
	list.clear();

	try {
		XmlListLoader loader(list);

		if((listType == TYPE_BZIP2) && (!downloadBuf.empty())) {
			FilteredInputStream<UnBZFilter, false> f(&mis);
			SimpleXMLReader(&loader).parse(f);
		} else {
			SimpleXMLReader(&loader).parse(mis);
		}
	} catch(const Exception&) {
		success = false;
		fire(FavoriteManagerListener::Corrupted(), fromHttp ? publicListServer : Util::emptyString);
	}

	if(fromHttp) {
		try {
			File f(Util::getHubListsPath() + Util::validateFileName(publicListServer), File::WRITE, File::CREATE | File::TRUNCATE);
			f.write(downloadBuf);
			f.close();
		} catch(const FileException&) { }
	}

	downloadBuf = Util::emptyString;
	
	return success;
}

// FAVORITE HUBS START
bool FavoriteManager::addFavoriteHub(const FavoriteHubEntryPtr& aEntry) {
	{
		WLock l(cs);
		auto i = getFavoriteHub(aEntry->getServer());
		if (i != favoriteHubs.end()) {
			return false;
		}

		favoriteHubs.push_back(aEntry);
	}

	fire(FavoriteManagerListener::FavoriteHubAdded(), aEntry);
	save();
	return true;
}

void FavoriteManager::onFavoriteHubUpdated(const FavoriteHubEntryPtr& aEntry) {
	// Update the connect state in case the address was changed
	auto client = ClientManager::getInstance()->getClient(aEntry->getServer());
	if (client) {
		aEntry->setConnectState(client->isConnected() ? FavoriteHubEntry::STATE_CONNECTED : FavoriteHubEntry::STATE_CONNECTING);
		aEntry->setCurrentHubToken(client->getClientId());
	} else {
		aEntry->setCurrentHubToken(0);
		aEntry->setConnectState(FavoriteHubEntry::STATE_DISCONNECTED);
	}

	save();
	fire(FavoriteManagerListener::FavoriteHubUpdated(), aEntry);
}

void FavoriteManager::autoConnect() {
	RecentHubEntryList hubs;

	{

		RLock l(cs);
		for (const auto& entry : favoriteHubs) {
			if (entry->getAutoConnect()) {
				RecentHubEntryPtr r = new RecentHubEntry(entry->getServer());
				r->setName(entry->getName());
				r->setDescription(entry->getDescription());
				hubs.emplace_back(r);
			}
		}
	}

	for (const auto& h : hubs) {
		ClientManager::getInstance()->createClient(h);
	}
}

bool FavoriteManager::removeFavoriteHub(ProfileToken aToken) {
	FavoriteHubEntryPtr entry = nullptr;

	{
		WLock l(cs);
		auto i = find_if(favoriteHubs, [aToken](const FavoriteHubEntryPtr& f) { return f->getToken() == aToken; });
		if (i == favoriteHubs.end()) {
			return false;
		}

		entry = *i;
		favoriteHubs.erase(i);
	}

	fire(FavoriteManagerListener::FavoriteHubRemoved(), entry);
	save();
	return true;
}

bool FavoriteManager::isUnique(const string& url, ProfileToken aToken) {
	auto i = getFavoriteHub(url);
	if (i == favoriteHubs.end())
		return true;

	return aToken == (*i)->getToken();
}

void FavoriteManager::on(ShareManagerListener::DefaultProfileChanged, ProfileToken aOldDefault, ProfileToken aNewDefault) noexcept {
	resetProfile(aOldDefault, aNewDefault, true);
}

void FavoriteManager::on(ShareManagerListener::ProfileRemoved, ProfileToken aProfile) noexcept {
	resetProfile(aProfile, HUB_SETTING_DEFAULT_INT, false);
}

int FavoriteManager::resetProfile(ProfileToken aResetToken, ProfileToken aDefaultProfile, bool nmdcOnly) {
	FavoriteHubEntryList updatedHubs;

	{
		RLock l(cs);
		for (const auto& fh : favoriteHubs) {
			if (fh->get(HubSettings::ShareProfile) == aResetToken) {
				if (!nmdcOnly || !fh->isAdcHub()) {
					fh->get(HubSettings::ShareProfile) = aDefaultProfile;
					updatedHubs.push_back(fh);
				}
			}
		}
	}

	for (const auto& fh : updatedHubs) {
		fire(FavoriteManagerListener::FavoriteHubUpdated(), fh);
	}


	// Remove later
	fire(FavoriteManagerListener::FavoriteHubsUpdated());
	return static_cast<int>(updatedHubs.size());
}

bool FavoriteManager::hasActiveHubs() const {
	return any_of(favoriteHubs.begin(), favoriteHubs.end(), [](const FavoriteHubEntryPtr& f) { return f->get(HubSettings::Connection) == SettingsManager::INCOMING_ACTIVE || f->get(HubSettings::Connection6) == SettingsManager::INCOMING_ACTIVE; });
}

// FAVORITE HUBS END

void FavoriteManager::save() {
	if(dontSave)
		return;

	RLock l(cs);
	try {
		SimpleXML xml;

		xml.addTag("Favorites");
		xml.stepIn();

		xml.addTag("CID", SETTING(PRIVATE_ID));

		xml.addTag("Hubs");
		xml.stepIn();

		for(auto& i: favHubGroups) {
			xml.addTag("Group");
			xml.addChildAttrib("Name", i.first);
			i.second.save(xml);
		}

		for(auto& i: favoriteHubs) {
			xml.addTag("Hub");
			xml.addChildAttrib("Name", i->getName());
			xml.addChildAttrib("Connect", i->getAutoConnect());
			xml.addChildAttrib("Description", i->getDescription());
			xml.addChildAttrib("Password", i->getPassword());
			xml.addChildAttrib("Server", i->getServer());
			xml.addChildAttrib("ChatUserSplit", i->getChatUserSplit());
			xml.addChildAttrib("StealthMode", i->getStealth());
			xml.addChildAttrib("UserListState", i->getUserListState());
			xml.addChildAttrib("HubFrameOrder",	i->getHeaderOrder());
			xml.addChildAttrib("HubFrameWidths", i->getHeaderWidths());
			xml.addChildAttrib("HubFrameVisible", i->getHeaderVisible());
			xml.addChildAttrib("FavNoPM", i->getFavNoPM());	
			xml.addChildAttrib("Group", i->getGroup());
			xml.addChildAttrib("Bottom", Util::toString(i->getBottom()));
			xml.addChildAttrib("Top", Util::toString(i->getTop()));
			xml.addChildAttrib("Right",	Util::toString(i->getRight()));
			xml.addChildAttrib("Left", Util::toString(i->getLeft()));
			xml.addChildAttrib("ShareProfile", i->get(HubSettings::ShareProfile));
			i->save(xml);
		}

		xml.stepOut();

		xml.addTag("Users");
		xml.stepIn();
		for(auto& i: users) {
			xml.addTag("User");
			xml.addChildAttrib("LastSeen", i.second.getLastSeen());
			xml.addChildAttrib("GrantSlot", i.second.isSet(FavoriteUser::FLAG_GRANTSLOT));
			xml.addChildAttrib("SuperUser", i.second.isSet(FavoriteUser::FLAG_SUPERUSER));
			xml.addChildAttrib("UserDescription", i.second.getDescription());
			xml.addChildAttrib("Nick", i.second.getNick());
			xml.addChildAttrib("URL", i.second.getUrl());
			xml.addChildAttrib("CID", i.first.toBase32());
		}
		xml.stepOut();

		xml.addTag("UserCommands");
		xml.stepIn();
		for(auto& i: userCommands) {
			if(!i.isSet(UserCommand::FLAG_NOSAVE)) {
				xml.addTag("UserCommand");
				xml.addChildAttrib("Type", i.getType());
				xml.addChildAttrib("Context", i.getCtx());
				xml.addChildAttrib("Name", i.getName());
				xml.addChildAttrib("Command", i.getCommand());
				xml.addChildAttrib("To", i.getTo());
				xml.addChildAttrib("Hub", i.getHub());
			}
		}
		xml.stepOut();

		//Favorite download to dirs
		xml.addTag("FavoriteDirs");
		xml.addChildAttrib("Version", 2);
		xml.stepIn();

		for(const auto& fde: favoriteDirs) {
			xml.addTag("Directory", fde.first);
			xml.addChildAttrib("Name", fde.first);
			xml.stepIn();
			for (const auto& t: fde.second) {
				xml.addTag("Target", t);
			}
			xml.stepOut();
		}
		xml.stepOut();

		xml.stepOut();


		SettingsManager::saveSettingFile(xml, CONFIG_DIR, CONFIG_FAV_NAME);
	} catch(const Exception& e) {
		dcdebug("FavoriteManager::save: %s\n", e.getError().c_str());
	}
}

void FavoriteManager::previewload(SimpleXML& aXml){
	aXml.resetCurrentChild();
	if(aXml.findChild("PreviewApps")) {
		aXml.stepIn();
		while(aXml.findChild("Application")) {					
			addPreviewApp(aXml.getChildAttrib("Name"), aXml.getChildAttrib("Application"), 
				aXml.getChildAttrib("Arguments"), aXml.getChildAttrib("Extension"));			
		}
		aXml.stepOut();
	}	
}

void FavoriteManager::previewsave(SimpleXML& aXml){
	aXml.addTag("PreviewApps");
	aXml.stepIn();
	for(const auto pa: previewApplications) {
		aXml.addTag("Application");
		aXml.addChildAttrib("Name", pa->getName());
		aXml.addChildAttrib("Application", pa->getApplication());
		aXml.addChildAttrib("Arguments", pa->getArguments());
		aXml.addChildAttrib("Extension", pa->getExtension());
	}
	aXml.stepOut();
}

void FavoriteManager::recentsave() {
	SimpleXML xml;

	xml.addTag("Recents");
	xml.stepIn();

	xml.addTag("Hubs");
	xml.stepIn();

	{
		RLock l(cs);
		for (const auto rhe : recentHubs) {
			xml.addTag("Hub");
			xml.addChildAttrib("Name", rhe->getName());
			xml.addChildAttrib("Description", rhe->getDescription());
			xml.addChildAttrib("Users", rhe->getUsers());
			xml.addChildAttrib("Shared", rhe->getShared());
			xml.addChildAttrib("Server", rhe->getServer());
		}
	}

	xml.stepOut();
	xml.stepOut();
		
	SettingsManager::saveSettingFile(xml, CONFIG_DIR, CONFIG_RECENTS_NAME);
}

void FavoriteManager::loadCID() {
	try {
		SimpleXML xml;
		SettingsManager::loadSettingFile(xml, CONFIG_DIR, CONFIG_FAV_NAME);

		if(xml.findChild("Favorites")) {
			xml.stepIn();
			if(xml.findChild("CID")) {
				xml.stepIn();
				SettingsManager::getInstance()->set(SettingsManager::PRIVATE_ID, xml.getData());
				xml.stepOut();
			}

			xml.stepOut();
		}
	} catch(const Exception& e) {
		LogManager::getInstance()->message(STRING_F(LOAD_FAILED_X, CONFIG_FAV_NAME % e.getError()), LogMessage::SEV_ERROR);
	}
}

void FavoriteManager::load() {
	
	// Add NMDC standard op commands
	static const char kickstr[] = 
		"$To: %[userNI] From: %[myNI] $<%[myNI]> You are being kicked because: %[kickline:Reason]|<%[myNI]> is kicking %[userNI] because: %[kickline:Reason]|$Kick %[userNI]|";
	addUserCommand(UserCommand::TYPE_RAW_ONCE, UserCommand::CONTEXT_USER | UserCommand::CONTEXT_SEARCH, UserCommand::FLAG_NOSAVE, 
		STRING(KICK_USER), kickstr, "", "op");
	static const char kickfilestr[] = 
		"$To: %[userNI] From: %[myNI] $<%[myNI]> You are being kicked because: %[kickline:Reason] %[fileFN]|<%[myNI]> is kicking %[userNI] because: %[kickline:Reason] %[fileFN]|$Kick %[userNI]|";
	addUserCommand(UserCommand::TYPE_RAW_ONCE, UserCommand::CONTEXT_SEARCH, UserCommand::FLAG_NOSAVE, 
		STRING(KICK_USER_FILE), kickfilestr, "", "op");
	static const char redirstr[] =
		"$OpForceMove $Who:%[userNI]$Where:%[line:Target Server]$Msg:%[line:Message]|";
	addUserCommand(UserCommand::TYPE_RAW_ONCE, UserCommand::CONTEXT_USER | UserCommand::CONTEXT_SEARCH, UserCommand::FLAG_NOSAVE, 
		STRING(REDIRECT_USER), redirstr, "", "op");

	try {
		SimpleXML xml;
		SettingsManager::loadSettingFile(xml, CONFIG_DIR, CONFIG_FAV_NAME, false); //we have migrated already when loading the CID
		if(xml.findChild("Favorites")) {
			xml.stepIn();
			load(xml);
			xml.stepOut();

			//we have load it fine now, so make a backup of a working favorites.xml
			auto f = Util::getPath(CONFIG_DIR) + CONFIG_FAV_NAME;
			File::deleteFile(f + ".bak");
			File::copyFile(f, f + ".bak");
		}
	} catch(const Exception& e) {
		LogManager::getInstance()->message(STRING_F(LOAD_FAILED_X, CONFIG_FAV_NAME % e.getError()), LogMessage::SEV_ERROR);
	}

	try {
		SimpleXML xml;
		SettingsManager::loadSettingFile(xml, CONFIG_DIR, CONFIG_RECENTS_NAME);
		if(xml.findChild("Recents")) {
			xml.stepIn();
			recentload(xml);
			xml.stepOut();
		}
	} catch(const Exception& e) {
		LogManager::getInstance()->message(STRING_F(LOAD_FAILED_X, CONFIG_RECENTS_NAME % e.getError()), LogMessage::SEV_ERROR);
	}
}

void FavoriteManager::load(SimpleXML& aXml) {
	dontSave = true;
	bool needSave = false;

	aXml.resetCurrentChild();
	if(aXml.findChild("Hubs")) {
		aXml.stepIn();

		while(aXml.findChild("Group")) {
			string name = aXml.getChildAttrib("Name");
			if(name.empty())
				continue;

			HubSettings settings;
			settings.load(aXml);
			favHubGroups[name] = std::move(settings);
		}

		aXml.resetCurrentChild();
		while(aXml.findChild("Hub")) {
			FavoriteHubEntryPtr e = new FavoriteHubEntry();
			e->setName(aXml.getChildAttrib("Name"));
			e->setAutoConnect(aXml.getBoolChildAttrib("Connect"));
			e->setDescription(aXml.getChildAttrib("Description"));
			e->setPassword(aXml.getChildAttrib("Password"));

			auto server = aXml.getChildAttrib("Server");
			if (server.empty()) {
				LogManager::getInstance()->message("A favorite hub with an empty address wasn't loaded: " + e->getName(), LogMessage::SEV_WARNING);
				continue;
			}

			// Remove failovers
			auto p = server.find(";");
			if (p != string::npos) {
				server = server.substr(0, p);
			}

			e->setServer(server);

			e->setChatUserSplit(aXml.getIntChildAttrib("ChatUserSplit"));
			e->setStealth(aXml.getBoolChildAttrib("StealthMode"));
			e->setUserListState(aXml.getBoolChildAttrib("UserListState"));
			e->setHeaderOrder(aXml.getChildAttrib("HubFrameOrder", SETTING(HUBFRAME_ORDER)));
			e->setHeaderWidths(aXml.getChildAttrib("HubFrameWidths", SETTING(HUBFRAME_WIDTHS)));
			e->setHeaderVisible(aXml.getChildAttrib("HubFrameVisible", SETTING(HUBFRAME_VISIBLE)));
			e->setBottom((uint16_t)aXml.getIntChildAttrib("Bottom") );
			e->setTop((uint16_t)	aXml.getIntChildAttrib("Top"));
			e->setRight((uint16_t)	aXml.getIntChildAttrib("Right"));
			e->setLeft((uint16_t)	aXml.getIntChildAttrib("Left"));
			e->setFavNoPM(aXml.getBoolChildAttrib("FavNoPM"));
			e->setGroup(aXml.getChildAttrib("Group"));
			if (aXml.getBoolChildAttrib("HideShare")) {
				// For compatibility with very old favorites
				e->get(HubSettings::ShareProfile) = SP_HIDDEN;
			}

			e->load(aXml);

			// Unset share profile for old NMDC hubs
			if (!e->isAdcHub() && e->get(HubSettings::ShareProfile) != SP_HIDDEN) {
				e->get(HubSettings::ShareProfile) = HUB_SETTING_DEFAULT_INT;
			}

			favoriteHubs.push_back(e);
		}

		aXml.stepOut();
	}
	aXml.resetCurrentChild();
	if(aXml.findChild("Users")) {
		aXml.stepIn();
		while(aXml.findChild("User")) {
			UserPtr u;
			const string& cid = aXml.getChildAttrib("CID");
			const string& nick = aXml.getChildAttrib("Nick");
			const string& hubUrl = aXml.getChildAttrib("URL");
			ClientManager* cm = ClientManager::getInstance();

			if(cid.length() != 39) {
				if(nick.empty() || hubUrl.empty())
					continue;
				u = cm->getUser(nick, hubUrl);
			} else {
				u = cm->getUser(CID(cid));
			}
			u->setFlag(User::FAVORITE);

			auto i = users.emplace(u->getCID(), FavoriteUser(u, nick, hubUrl, cid)).first;
			{
				WLock(cm->getCS());
				cm->addOfflineUser(u, nick, hubUrl);
			}

			if(aXml.getBoolChildAttrib("GrantSlot"))
				i->second.setFlag(FavoriteUser::FLAG_GRANTSLOT);
			if (aXml.getBoolChildAttrib("SuperUser"))
				i->second.setFlag(FavoriteUser::FLAG_SUPERUSER);

			i->second.setLastSeen((uint32_t)aXml.getIntChildAttrib("LastSeen"));
			i->second.setDescription(aXml.getChildAttrib("UserDescription"));

		}
		aXml.stepOut();
	}

	aXml.resetCurrentChild();
	if(aXml.findChild("UserCommands")) {
		aXml.stepIn();
		while(aXml.findChild("UserCommand")) {
			addUserCommand(aXml.getIntChildAttrib("Type"), aXml.getIntChildAttrib("Context"), 0, aXml.getChildAttrib("Name"),
				aXml.getChildAttrib("Command"), aXml.getChildAttrib("To"), aXml.getChildAttrib("Hub"));
		}
		aXml.stepOut();
	}

	//Favorite download to dirs
	aXml.resetCurrentChild();
	if(aXml.findChild("FavoriteDirs")) {
		string version = aXml.getChildAttrib("Version");
		aXml.stepIn();
		if (version.empty() || Util::toInt(version) < 2) {
			//convert old directories
			while(aXml.findChild("Directory")) {
				string virt = aXml.getChildAttrib("Name");

				StringList targets;
				targets.push_back(aXml.getChildData());
				FavoriteManager::getInstance()->addFavoriteDir(virt, targets);
			}
			needSave = true;
		} else {
			while(aXml.findChild("Directory")) {
				string name = aXml.getChildAttrib("Name");
				if (!name.empty()) {
					aXml.stepIn();
					StringList targets;
					while(aXml.findChild("Target")) {
						aXml.stepIn();
						string path = aXml.getData();
						if( path[ path.length() -1 ] != PATH_SEPARATOR ) {
							path += PATH_SEPARATOR;
						}
						if (find(targets.begin(), targets.end(), path) == targets.end()) {
							targets.push_back(path);
						}
						aXml.stepOut();
					}
					if (!targets.empty()) {
						FavoriteManager::getInstance()->addFavoriteDir(name, targets);
					}
					aXml.stepOut();
				}
			}
		}
		aXml.stepOut();
	}

	dontSave = false;
	if(needSave)
		save();
}
	
FavoriteHubEntryList FavoriteManager::getFavoriteHubs(const string& group) const {
	FavoriteHubEntryList ret;
	copy_if(favoriteHubs.begin(), favoriteHubs.end(), back_inserter(ret), [&group](const FavoriteHubEntryPtr& f) { return Util::stricmp(f->getGroup(), group) == 0; });
	return ret;
}

void FavoriteManager::setHubSetting(const string& aUrl, HubSettings::HubBoolSetting aSetting, bool newValue) {
	RLock l(cs);
	auto p = getFavoriteHub(aUrl);
	if (p != favoriteHubs.end()) {
		(*p)->get(aSetting) = newValue;
	}
}

bool FavoriteManager::hasSlot(const UserPtr& aUser) const { 
	RLock l(cs);
	auto i = users.find(aUser->getCID());
	if(i == users.end())
		return false;
	return i->second.isSet(FavoriteUser::FLAG_GRANTSLOT);
}

time_t FavoriteManager::getLastSeen(const UserPtr& aUser) const { 
	RLock l(cs);
	auto i = users.find(aUser->getCID());
	if(i == users.end())
		return 0;
	return i->second.getLastSeen();
}

void FavoriteManager::setAutoGrant(const UserPtr& aUser, bool grant) {
	{
		RLock l(cs);
		auto i = users.find(aUser->getCID());
		if(i == users.end())
			return;
		if(grant)
			i->second.setFlag(FavoriteUser::FLAG_GRANTSLOT);
		else
			i->second.unsetFlag(FavoriteUser::FLAG_GRANTSLOT);
	}

	save();
}
void FavoriteManager::setUserDescription(const UserPtr& aUser, const string& description) {
	{
		RLock l(cs);
		auto i = users.find(aUser->getCID());
		if(i == users.end())
			return;
		i->second.setDescription(description);
	}

	save();
}

void FavoriteManager::recentload(SimpleXML& aXml) {
	aXml.resetCurrentChild();
	if(aXml.findChild("Hubs")) {
		aXml.stepIn();
		while(aXml.findChild("Hub")) {
			RecentHubEntryPtr e = new RecentHubEntry(aXml.getChildAttrib("Server"));
			e->setName(aXml.getChildAttrib("Name"));
			e->setDescription(aXml.getChildAttrib("Description"));
			e->setUsers(aXml.getChildAttrib("Users"));
			e->setShared(aXml.getChildAttrib("Shared"));
			recentHubs.push_back(e);
		}
		aXml.stepOut();
	}
}

StringList FavoriteManager::getHubLists() {
	StringTokenizer<string> lists(SETTING(HUBLIST_SERVERS), ';');
	return lists.getTokens();
}

FavoriteHubEntryPtr FavoriteManager::getFavoriteHubEntry(const string& aServer) const {
	RLock l(cs);
	auto p = getFavoriteHub(aServer);
	return p != favoriteHubs.end() ? *p : nullptr;
}

FavoriteHubEntryPtr FavoriteManager::getFavoriteHubEntry(const ProfileToken& aToken) const {
	RLock l(cs);
	auto p = getFavoriteHub(aToken);
	return p != favoriteHubs.end() ? *p : nullptr;
}

void FavoriteManager::mergeHubSettings(const FavoriteHubEntryPtr& entry, HubSettings& settings) const {
	// apply group settings first.
	const string& name = entry->getGroup();
	if(!name.empty()) {
		auto group = favHubGroups.find(name);
		if(group != favHubGroups.end())
			settings.merge(group->second);
	}

	// apply fav entry settings next.
	settings.merge(*entry);
}

FavoriteHubEntryList::const_iterator FavoriteManager::getFavoriteHub(const string& aServer) const {
	//find by the primary address
	return find_if(favoriteHubs, [&aServer](const FavoriteHubEntryPtr& f) { return Util::stricmp(f->getServer(), aServer) == 0; });
}

FavoriteHubEntryList::const_iterator FavoriteManager::getFavoriteHub(ProfileToken aToken) const {
	return find_if(favoriteHubs, [aToken](const FavoriteHubEntryPtr& f) { return f->getToken() == aToken; });
}

RecentHubEntryList::const_iterator FavoriteManager::getRecentHub(const string& aServer) const {
	return find_if(recentHubs, [&aServer](const RecentHubEntryPtr& rhe) { return Util::stricmp(rhe->getServer(), aServer) == 0; });
}

void FavoriteManager::setHubList(int aHubList) {
	lastServer = aHubList;
	refresh();
}

RecentHubEntryPtr FavoriteManager::getRecentHubEntry(const string& aServer) {
	RLock l(cs);
	auto p = getRecentHub(aServer);
	return p == recentHubs.end() ? nullptr : *p;
}

RecentHubEntryList FavoriteManager::searchRecentHubs(const string& aPattern, size_t aMaxResults) const noexcept {
	auto search = RelevancySearch<RecentHubEntryPtr>(aPattern, [](const RecentHubEntryPtr& aHub) {
		return aHub->getName();
	});

	{
		RLock l(cs);
		for (const auto& hub : recentHubs) {
			search.match(hub);
		}
	}

	return search.getResults(aMaxResults);
}

void FavoriteManager::refresh(bool forceDownload /* = false */) {
	StringList sl = getHubLists();
	if(sl.empty())
		return;
	publicListServer = sl[(lastServer) % sl.size()];
	if (Util::strnicmp(publicListServer.c_str(), "http://", 7) != 0) {
		lastServer++;
		return;
	}

	if(!forceDownload) {
		string path = Util::getHubListsPath() + Util::validateFileName(publicListServer);
		if(File::getSize(path) > 0) {
			useHttp = false;
			string fileDate;
			{
				WLock l(cs);
				publicListMatrix[publicListServer].clear();
			}
			listType = (Util::stricmp(path.substr(path.size() - 4), ".bz2") == 0) ? TYPE_BZIP2 : TYPE_NORMAL;
			try {
				File cached(path, File::READ, File::OPEN);
				downloadBuf = cached.read();
				char buf[20];
				time_t fd = cached.getLastModified();
				if (strftime(buf, 20, "%x", localtime(&fd))) {
					fileDate = string(buf);
				}
			} catch(const FileException&) {
				downloadBuf = Util::emptyString;
			}
			if(!downloadBuf.empty()) {
				if (onHttpFinished(false)) {
					fire(FavoriteManagerListener::LoadedFromCache(), publicListServer, fileDate);
				}		
				return;
			}
		}
	}

	if(!running) {
		useHttp = true;
		{
			WLock l(cs);
			publicListMatrix[publicListServer].clear();
		}
		fire(FavoriteManagerListener::DownloadStarting(), publicListServer);
		if(!c)
			c = new HttpConnection();
		c->addListener(this);
		c->downloadFile(publicListServer);
		running = true;
	}
}

UserCommand::List FavoriteManager::getUserCommands(int ctx, const StringList& hubs, bool& op) {
	vector<bool> isOp(hubs.size());

	for(size_t i = 0; i < hubs.size(); ++i) {
		if(ClientManager::getInstance()->isOp(ClientManager::getInstance()->getMe(), hubs[i])) {
			isOp[i] = true;
			op = true; // ugly hack
		}
	}

	RLock l(cs);
	UserCommand::List lst;
	for(const auto& uc: userCommands) {
		if(!(uc.getCtx() & ctx)) {
			continue;
		}

		for(size_t j = 0; j < hubs.size(); ++j) {
			const string& hub = hubs[j];
			bool hubAdc = hub.compare(0, 6, "adc://") == 0 || hub.compare(0, 7, "adcs://") == 0;
			bool commandAdc = uc.getHub().compare(0, 6, "adc://") == 0 || uc.getHub().compare(0, 7, "adcs://") == 0;
			if(hubAdc && commandAdc) {
				if((uc.getHub() == "adc://" || uc.getHub() == "adcs://") ||
					((uc.getHub() == "adc://op" || uc.getHub() == "adcs://op") && isOp[j]) ||
					(uc.getHub() == hub) )
				{
					lst.push_back(uc);
					break;
				}
			} else if((!hubAdc && !commandAdc) || uc.isChat()) {
				if((uc.getHub().length() == 0) || 
					(uc.getHub() == "op" && isOp[j]) ||
					(uc.getHub() == hub) )
				{
					lst.push_back(uc);
					break;
				}
			}
		}
	}
	return lst;
}

// HttpConnectionListener
void FavoriteManager::on(Data, HttpConnection*, const uint8_t* buf, size_t len) noexcept { 
	if(useHttp)
		downloadBuf.append((const char*)buf, len);
}

void FavoriteManager::on(Failed, HttpConnection*, const string& aLine) noexcept { 
	c->removeListener(this);
	lastServer++;
	running = false;
	if(useHttp){
		downloadBuf = Util::emptyString;
		fire(FavoriteManagerListener::DownloadFailed(), aLine);
	}
}
void FavoriteManager::on(Complete, HttpConnection*, const string& aLine, bool fromCoral) noexcept {
	bool parseSuccess = false;
	c->removeListener(this);
	if(useHttp) {
		if(c->getMimeType() == "application/x-bzip2")
			listType = TYPE_BZIP2;
		parseSuccess = onHttpFinished(true);
	}	
	running = false;
	if(parseSuccess) {
		fire(FavoriteManagerListener::DownloadFinished(), aLine, fromCoral);
	}
}
void FavoriteManager::on(Redirected, HttpConnection*, const string& aLine) noexcept { 
	if(useHttp)
		fire(FavoriteManagerListener::DownloadStarting(), aLine);
}

void FavoriteManager::on(Retried, HttpConnection*, const bool Connected) noexcept {
	if (Connected)
		downloadBuf = Util::emptyString;
}

void FavoriteManager::on(UserDisconnected, const UserPtr& user, bool wentOffline) noexcept {
	bool isFav = false;
	{
		RLock l(cs);
		auto i = users.find(user->getCID());
		if(i != users.end()) {
			isFav = true;
			if (wentOffline) {
				i->second.setLastSeen(GET_TIME());
			}
		}
	}

	if(isFav)
		fire(FavoriteManagerListener::StatusChanged(), user);
}

void FavoriteManager::on(UserConnected, const OnlineUser& aUser, bool /*wasOffline*/) noexcept {
	UserPtr user = aUser.getUser();

	if(user->isSet(User::FAVORITE))
		fire(FavoriteManagerListener::StatusChanged(), user);
}

void FavoriteManager::on(ClientManagerListener::ClientCreated, const ClientPtr& aClient) noexcept {
	onConnectStateChanged(aClient, FavoriteHubEntry::STATE_CONNECTING);
}

void FavoriteManager::on(ClientManagerListener::ClientConnected, const ClientPtr& aClient) noexcept {
	onConnectStateChanged(aClient, FavoriteHubEntry::STATE_CONNECTED);
}

void FavoriteManager::on(ClientManagerListener::ClientRemoved, const ClientPtr& aClient) noexcept {
	onConnectStateChanged(aClient, FavoriteHubEntry::STATE_DISCONNECTED);
}

void FavoriteManager::on(ClientManagerListener::ClientRedirected, const ClientPtr& aOldClient, const ClientPtr& aNewClient) noexcept {
	onConnectStateChanged(aOldClient, FavoriteHubEntry::STATE_DISCONNECTED);
	onConnectStateChanged(aNewClient, FavoriteHubEntry::STATE_CONNECTING);
}

void FavoriteManager::onConnectStateChanged(const ClientPtr& aClient, FavoriteHubEntry::ConnectState aState) noexcept {
	auto hub = getFavoriteHubEntry(aClient->getHubUrl());
	if (hub) {
		hub->setConnectState(aState);
		if (aState == FavoriteHubEntry::STATE_DISCONNECTED) {
			hub->setCurrentHubToken(0);
		} else {
			hub->setCurrentHubToken(aClient->getClientId());
		}

		fire(FavoriteManagerListener::FavoriteHubUpdated(), hub);
	}
}

} // namespace dcpp
