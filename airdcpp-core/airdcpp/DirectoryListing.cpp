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

#include "DirectoryListing.h"

#include "ADLSearch.h"
#include "AutoSearchManager.h"
#include "Bundle.h"
#include "BZUtils.h"
#include "DirectoryListingManager.h"
#include "Download.h"
#include "DownloadManager.h"
#include "FilteredFile.h"
#include "QueueManager.h"
#include "ResourceManager.h"
#include "ShareManager.h"
#include "SimpleXML.h"
#include "SimpleXMLReader.h"
#include "StringTokenizer.h"
#include "User.h"
#include "ScopedFunctor.h"


namespace dcpp {

using boost::range::for_each;
using boost::range::find_if;

DirectoryListing::DirectoryListing(const HintedUser& aUser, bool aPartial, const string& aFileName, bool aIsClientView, bool aIsOwnList) :
	hintedUser(aUser), root(new Directory(nullptr, Util::emptyString, Directory::TYPE_INCOMPLETE_NOCHILD, 0)), partialList(aPartial), isOwnList(aIsOwnList), fileName(aFileName),
	isClientView(aIsClientView), matchADL(SETTING(USE_ADLS) && !aPartial), 
	tasks(isClientView, Thread::NORMAL, std::bind(&DirectoryListing::dispatch, this, std::placeholders::_1))
{
	running.clear();

	ClientManager::getInstance()->addListener(this);
	if (isOwnList) {
		ShareManager::getInstance()->addListener(this);
	}
}

DirectoryListing::~DirectoryListing() {
	dcdebug("Filelist deleted\n");
	ClientManager::getInstance()->removeListener(this);
	ShareManager::getInstance()->removeListener(this);
}

bool DirectoryListing::isMyCID() const noexcept {
	return hintedUser.user == ClientManager::getInstance()->getMe();
}


bool DirectoryListing::Directory::Sort::operator()(const Ptr& a, const Ptr& b) const {
	return compare(a->getName(), b->getName()) < 0;
}

bool DirectoryListing::File::Sort::operator()(const Ptr& a, const Ptr& b) const {
	return compare(a->getName(), b->getName()) < 0;
}

string DirectoryListing::getNick(bool firstOnly) const noexcept {
	string ret;
	if (!hintedUser.user->isOnline()) {
		if (isOwnList) {
			ret = SETTING(NICK);
		} else if (!partialList) {
			ret = DirectoryListing::getNickFromFilename(fileName);
		}
	}

	if (ret.empty()) {
		if (firstOnly) {
			ret = ClientManager::getInstance()->getNick(hintedUser.user, hintedUser.hint, true);
		} else {
			ret = ClientManager::getInstance()->getFormatedNicks(hintedUser);
		}
	}

	return ret;
}

void DirectoryListing::setHubUrl(const string& newUrl, bool) noexcept {
	hintedUser.hint = newUrl;
	fire(DirectoryListingListener::UserUpdated());
}

void stripExtensions(string& name) noexcept {
	if(Util::stricmp(name.c_str() + name.length() - 4, ".bz2") == 0) {
		name.erase(name.length() - 4);
	}

	if(Util::stricmp(name.c_str() + name.length() - 4, ".xml") == 0) {
		name.erase(name.length() - 4);
	}
}

string DirectoryListing::getNickFromFilename(const string& fileName) noexcept {
	// General file list name format: [username].[CID].[xml|xml.bz2]

	string name = Util::getFileName(fileName);

	// Strip off any extensions
	stripExtensions(name);

	// Find CID
	string::size_type i = name.rfind('.');
	if(i == string::npos) {
		return STRING(UNKNOWN);
	}

	return name.substr(0, i);
}

UserPtr DirectoryListing::getUserFromFilename(const string& fileName) noexcept {
	// General file list name format: [username].[CID].[xml|xml.bz2]

	string name = Util::getFileName(fileName);

	// Strip off any extensions
	stripExtensions(name);

	// Find CID
	string::size_type i = name.rfind('.');
	if(i == string::npos) {
		return UserPtr();
	}

	size_t n = name.length() - (i + 1);
	// CID's always 39 chars long...
	if(n != 39)
		return UserPtr();

	CID cid(name.substr(i + 1));
	if(!cid)
		return UserPtr();

	return ClientManager::getInstance()->getUser(cid);
}

bool DirectoryListing::supportsASCH() const noexcept {
	return !partialList || isOwnList || hintedUser.user->isSet(User::ASCH);
}

void DirectoryListing::onStateChanged() noexcept {
	fire(DirectoryListingListener::StateChanged());
}

void DirectoryListing::loadFile() throw(Exception, AbortException) {
	if (isOwnList) {
		loadShareDirectory(Util::emptyString, true);
	} else {

		// For now, we detect type by ending...
		string ext = Util::getFileExt(fileName);

		dcpp::File ff(fileName, dcpp::File::READ, dcpp::File::OPEN);
		root->setUpdateDate(ff.getLastModified());
		if(Util::stricmp(ext, ".bz2") == 0) {
			FilteredInputStream<UnBZFilter, false> f(&ff);
			loadXML(f, false, "/", ff.getLastModified());
		} else if(Util::stricmp(ext, ".xml") == 0) {
			loadXML(ff, false, "/", ff.getLastModified());
		}
	}
}

class ListLoader : public SimpleXMLReader::CallBack {
public:
	ListLoader(DirectoryListing* aList, DirectoryListing::Directory* root, const string& aBase, bool aUpdating, const UserPtr& aUser, bool aCheckDupe, bool aPartialList, time_t aListDate) : 
	  list(aList), cur(root), base(aBase), inListing(false), updating(aUpdating), user(aUser), checkDupe(aCheckDupe), partialList(aPartialList), dirsLoaded(0), listDate(aListDate) { 
	}

	virtual ~ListLoader() { }

	void startTag(const string& name, StringPairList& attribs, bool simple);
	void endTag(const string& name);

	//const string& getBase() const { return base; }
	int getLoadedDirs() { return dirsLoaded; }
private:
	DirectoryListing* list;
	DirectoryListing::Directory* cur;
	UserPtr user;

	string baseLower;
	string base;
	bool inListing;
	bool updating;
	bool checkDupe;
	bool partialList;
	int dirsLoaded;
	time_t listDate;
};

int DirectoryListing::updateXML(const string& xml, const string& aBase) throw(AbortException) {
	MemoryInputStream mis(xml);
	return loadXML(mis, true, aBase);
}

int DirectoryListing::loadXML(InputStream& is, bool updating, const string& aBase, time_t aListDate) throw(AbortException) {
	ListLoader ll(this, root.get(), aBase, updating, getUser(), !isOwnList && isClientView && SETTING(DUPES_IN_FILELIST), partialList, aListDate);
	try {
		dcpp::SimpleXMLReader(&ll).parse(is);
	} catch(SimpleXMLException& e) {
		//Better to abort and show the error, than just leave it hanging.
		LogManager::getInstance()->message("Error in Filelist loading: "  + e.getError() + ". User: [ " +  
			getNick(false) + " ]", LogMessage::SEV_ERROR);
		//dcdebug("DirectoryListing loadxml error: %s", e.getError());
	}
	return ll.getLoadedDirs();
}

static const string sFileListing = "FileListing";
static const string sBase = "Base";
static const string sBaseDate = "BaseDate";
static const string sGenerator = "Generator";
static const string sDirectory = "Directory";
static const string sIncomplete = "Incomplete";
static const string sChildren = "Children";
static const string sFile = "File";
static const string sName = "Name";
static const string sSize = "Size";
static const string sTTH = "TTH";
static const string sDate = "Date";
void ListLoader::startTag(const string& name, StringPairList& attribs, bool simple) {
	if(list->getClosing()) {
		throw AbortException();
	}

	if(inListing) {
		if(name == sFile) {
			const string& n = getAttrib(attribs, sName, 0);
			if(n.empty())
				return;
			const string& s = getAttrib(attribs, sSize, 1);
			if(s.empty())
				return;
			auto size = Util::toInt64(s);

			const string& h = getAttrib(attribs, sTTH, 2);
			if(h.empty() && !SettingsManager::lanMode)
				return;		
			TTHValue tth(h); /// @todo verify validity?

			auto f = new DirectoryListing::File(cur, n, size, tth, checkDupe, Util::toUInt32(getAttrib(attribs, sDate, 3)));
			cur->files.push_back(f);
		} else if(name == sDirectory) {
			const string& n = getAttrib(attribs, sName, 0);
			if(n.empty()) {
				throw SimpleXMLException("Directory missing name attribute");
			}

			bool incomp = getAttrib(attribs, sIncomplete, 1) == "1";
			bool children = getAttrib(attribs, sChildren, 2) == "1";

			const string& size = getAttrib(attribs, sSize, 2);
			const string& date = getAttrib(attribs, sDate, 3);

			DirectoryListing::Directory::Ptr d = nullptr;
			if(updating) {
				dirsLoaded++;
				auto s =  list->baseDirs.find(baseLower + Text::toLower(n) + '/');
				if (s != list->baseDirs.end()) {
					d = s->second.first;
				}
			}

			if(!d) {
				d = new DirectoryListing::Directory(cur, n, incomp ? (children ? DirectoryListing::Directory::TYPE_INCOMPLETE_CHILD : DirectoryListing::Directory::TYPE_INCOMPLETE_NOCHILD) : 
					DirectoryListing::Directory::TYPE_NORMAL, listDate, (partialList && checkDupe), size, Util::toUInt32(date));
				cur->directories.push_back(d);
				if (updating && !incomp)
					list->baseDirs[baseLower + Text::toLower(n) + '/'] = { d, true }; //recursive partial lists
			} else {
				if(!incomp) {
					d->setComplete();
				}
				d->setRemoteDate(Util::toUInt32(date));
			}
			cur = d.get();
			if (updating && cur->isComplete())
				baseLower += Text::toLower(n) + '/';

			if(simple) {
				// To handle <Directory Name="..." />
				endTag(name);
			}
		}
	} else if(name == sFileListing) {
		if (updating) {
			const string& b = getAttrib(attribs, sBase, 2);
			if(b.size() >= 1 && b[0] == '/' && b[b.size()-1] == '/') {
				base = b;
				if (b != base) 
					throw AbortException("The base directory specified in the file list (" + b + ") doesn't match with the excepted base (" + base + ")");
			}
			const string& date = getAttrib(attribs, sBaseDate, 3);

			StringList sl = StringTokenizer<string>(base.substr(1), '/').getTokens();
			for(const auto& curDirName: sl) {
				auto s = find_if(cur->directories, [&curDirName](const DirectoryListing::Directory::Ptr& dir) { return dir->getName() == curDirName; });
				if (s == cur->directories.end()) {
					auto d = new DirectoryListing::Directory(cur, curDirName, DirectoryListing::Directory::TYPE_INCOMPLETE_CHILD, listDate, true);
					cur->directories.push_back(d);
					list->baseDirs[Text::toLower(Util::toAdcFile(d->getPath()))] = { d, false };
					cur = d;
				} else {
					cur = (*s).get();
				}
			}

			baseLower = Text::toLower(base);
			auto& p = list->baseDirs[baseLower];

			//set the dir as visited
			p.second = true;

			cur->setUpdateDate(listDate);
			cur->setRemoteDate(Util::toUInt32(date));
		}

		//set the root complete only after we have finished loading (will prevent possible problems like the GUI counting the size for this folder)
		inListing = true;

		if(simple) {
			// To handle <Directory Name="..." />
			endTag(name);
		}
	}
}

void ListLoader::endTag(const string& name) {
	if(inListing) {
		if(name == sDirectory) {
			if (updating && cur->isComplete())
				baseLower = baseLower.substr(0, baseLower.length()-cur->getName().length()-1);
			cur = cur->getParent();
		} else if(name == sFileListing) {
			// cur should be root now, set it complete
			cur->setComplete();
			inListing = false;
		}
	}
}

DirectoryListing::File::File(Directory* aDir, const string& aName, int64_t aSize, const TTHValue& aTTH, bool checkDupe, time_t aRemoteDate) noexcept : 
	name(aName), size(aSize), parent(aDir), tthRoot(aTTH), remoteDate(aRemoteDate) {

	if (checkDupe && size > 0) {
		dupe = AirUtil::checkFileDupe(tthRoot);
	}
}

DirectoryListing::File::File(const File& rhs, bool _adls) noexcept : name(rhs.name), size(rhs.size), parent(rhs.parent), tthRoot(rhs.tthRoot), adls(_adls), dupe(rhs.dupe), remoteDate(rhs.remoteDate)
{
}

DirectoryListing::Directory::Directory(Directory* aParent, const string& aName, Directory::DirType aType, time_t aUpdateDate, bool checkDupe, const string& aSize, time_t aRemoteDate /*0*/)
	: name(aName), parent(aParent), type(aType), remoteDate(aRemoteDate), updateDate(aUpdateDate) {

	if (!aSize.empty()) {
		partialSize = Util::toInt64(aSize);
	}

	if (checkDupe) {
		dupe = AirUtil::checkDirDupe(getPath(), partialSize);
	}
}

void DirectoryListing::Directory::search(OrderedStringSet& aResults, SearchQuery& aStrings) const noexcept {
	if (getAdls())
		return;

	if(aStrings.matchesDirectory(name)) {
		auto path = parent ? parent->getPath() : Util::emptyString;
		auto res = find(aResults, path);
		if (res == aResults.end() && aStrings.matchesSize(getTotalSize(false))) {
			aResults.insert(path);
		}
	}

	for(auto& f: files) {
		if(aStrings.matchesFile(f->getName(), f->getSize(), f->getRemoteDate(), f->getTTH())) {
			aResults.insert(getPath());
			break;
		}
	}

	for(const auto& d: directories) {
		d->search(aResults, aStrings);
		if (aResults.size() >= aStrings.maxResults) return;
	}
}

bool DirectoryListing::Directory::findIncomplete() const noexcept {
	/* Recursive check for incomplete dirs */
	if(!isComplete()) {
		return true;
	}
	return find_if(directories, [](const Directory::Ptr& dir) { return dir->findIncomplete(); }) != directories.end();
}

void DirectoryListing::Directory::download(const string& aTarget, BundleFileInfo::List& aFiles) noexcept{
	// First, recurse over the directories
	sort(directories.begin(), directories.end(), Directory::Sort());
	for(auto& d: directories) {
		d->download(aTarget + d->getName() + PATH_SEPARATOR, aFiles);
	}

	// Then add the files
	sort(files.begin(), files.end(), File::Sort());
	for(const auto& f: files) {
		aFiles.emplace_back(aTarget + f->getName(), f->getTTH(), f->getSize());
	}
}

bool DirectoryListing::createBundle(Directory::Ptr& aDir, const string& aTarget, QueueItemBase::Priority prio, ProfileToken aAutoSearch) noexcept {
	BundleFileInfo::List aFiles;
	aDir->download(Util::emptyString, aFiles);

	if (aFiles.empty() || (SETTING(SKIP_ZERO_BYTE) && none_of(aFiles.begin(), aFiles.end(), [](const BundleFileInfo& aFile) { return aFile.size > 0; }))) {
		fire(DirectoryListingListener::UpdateStatusMessage(), STRING(DIR_EMPTY) + " " + aDir->getName());
		return false;
	}

	string errorMsg;
	BundlePtr b = nullptr;
	try {
		b = QueueManager::getInstance()->createDirectoryBundle(aTarget, hintedUser.user == ClientManager::getInstance()->getMe() && !isOwnList ? HintedUser() : hintedUser,
			aFiles, prio, aDir->getRemoteDate(), errorMsg);
	} catch (const std::bad_alloc&) {
		LogManager::getInstance()->message(STRING_F(BUNDLE_CREATION_FAILED, aTarget % STRING(OUT_OF_MEMORY)), LogMessage::SEV_ERROR);
		return false;
	}

	if (!errorMsg.empty()) {
		if (aAutoSearch == 0) {
			LogManager::getInstance()->message(STRING_F(ADD_BUNDLE_ERRORS_OCC, aTarget % getNick(false) % errorMsg), LogMessage::SEV_WARNING);
		} else {
			AutoSearchManager::getInstance()->onBundleError(aAutoSearch, errorMsg, aTarget, hintedUser);
		}
	}

	if (b) {
		if (aAutoSearch > 0) {
			AutoSearchManager::getInstance()->onBundleCreated(b, aAutoSearch);
		}
		return true;
	}
	return false;
}

bool DirectoryListing::downloadDirImpl(Directory::Ptr& aDir, const string& aTarget, QueueItemBase::Priority prio, ProfileToken aAutoSearch) noexcept {
	dcassert(!aDir->findIncomplete());

	/* Check if this is a root dir containing release dirs */
	boost::regex reg;
	reg.assign(AirUtil::getReleaseRegBasic());
	if (!boost::regex_match(aDir->getName(), reg) && aDir->files.empty() && !aDir->directories.empty() &&
		all_of(aDir->directories.begin(), aDir->directories.end(), [&reg](const Directory::Ptr& d) { return boost::regex_match(d->getName(), reg); })) {
			
		/* Create bundles from each subfolder */
		bool queued = false;
		for(auto& d: aDir->directories) {
			if (createBundle(d, aTarget + d->getName() + PATH_SEPARATOR, prio, aAutoSearch))
				queued = true;
		}
		return queued;
	}

	return createBundle(aDir, aTarget, prio, aAutoSearch);
}

bool DirectoryListing::downloadDir(const string& aDir, const string& aTarget, QueueItemBase::Priority prio, ProfileToken aAutoSearch) noexcept {
	dcassert(aDir.size() > 2);
	dcassert(aDir[aDir.size() - 1] == '\\'); // This should not be PATH_SEPARATOR
	auto d = findDirectory(aDir, root);
	if(d)
		return downloadDirImpl(d, aTarget, prio, aAutoSearch);
	return false;
}

int64_t DirectoryListing::getDirSize(const string& aDir) const noexcept {
	dcassert(aDir.size() > 2);
	dcassert(aDir.empty() || aDir[aDir.size() - 1] == '\\'); // This should not be PATH_SEPARATOR
	auto d = findDirectory(aDir, root);
	if(d)
		return d->getTotalSize(false);
	return 0;
}

void DirectoryListing::openFile(const File::Ptr& aFile, bool aIsClientView) const throw(QueueException, FileException) {
	QueueManager::getInstance()->addOpenedItem(aFile->getName(), aFile->getSize(), aFile->getTTH(), hintedUser, aIsClientView);
}

DirectoryListing::Directory::Ptr DirectoryListing::findDirectory(const string& aName, const Directory::Ptr& current) const noexcept {
	if (aName.empty())
		return root;

	string::size_type end = aName.find('\\');
	dcassert(end != string::npos);
	string name = aName.substr(0, end);

	auto i = find(current->directories.begin(), current->directories.end(), name);
	if(i != current->directories.end()) {
		if(end == (aName.size() - 1))
			return *i;
		else
			return findDirectory(aName.substr(end + 1), *i);
	}
	return nullptr;
}

void DirectoryListing::Directory::findFiles(const boost::regex& aReg, File::List& aResults) const noexcept {
	copy_if(files.begin(), files.end(), back_inserter(aResults), [&aReg](const File::Ptr& df) { return boost::regex_match(df->getName(), aReg); });

	for(auto d: directories)
		d->findFiles(aReg, aResults); 
}

void DirectoryListing::findNfoImpl(const string& aPath, bool aAllowQueueList, DupeOpenF aDupeF) noexcept {
	auto dir = findDirectory(aPath, root);
	if (getIsOwnList()) {
		if (!aDupeF) {
			return;
		}

		try {
			SearchResultList results;
			auto s = unique_ptr<SearchQuery>(SearchQuery::getSearch(Util::emptyString, Util::emptyString, 0, SearchManager::TYPE_ANY, SearchManager::SIZE_DONTCARE, { ".nfo" }, SearchQuery::MATCH_NAME, false, 10));
			ShareManager::getInstance()->search(results, *s.get(), Util::toInt(getFileName()), ClientManager::getInstance()->getMyCID(), Util::toAdcFile(aPath));

			if (!results.empty()) {
				auto paths = AirUtil::getDupePaths(DUPE_SHARE, results.front()->getTTH());
				if (!paths.empty()) {
					aDupeF(paths.front());
				}

				return;
			}
		} catch (...) {
		
		}
	} else if ((!dir || !dir->isComplete() || dir->findIncomplete())) {
		if (!aAllowQueueList) {
			// Don't try to queue the same list over and over again if it's malformed
			return;
		}

		try {
			QueueManager::getInstance()->addList(hintedUser, QueueItem::FLAG_VIEW_NFO | QueueItem::FLAG_PARTIAL_LIST | QueueItem::FLAG_RECURSIVE_LIST, dir->getPath());
		}
		catch (const Exception&) {

		}

		return;
	} else {
		boost::regex reg;
		reg.assign("(.+\\.nfo)", boost::regex_constants::icase);
		File::List results;
		dir->findFiles(reg, results);

		if (!results.empty()) {
			try {
				openFile(results.front(), !SETTING(NFO_EXTERNAL));
			} catch (const Exception&) {
			
			}
			return;
		}
	}

	LogManager::getInstance()->message(dir->getName() + ": " + STRING(NO_NFO_FOUND), LogMessage::SEV_NOTIFY);
}

struct HashContained {
	HashContained(const DirectoryListing::Directory::TTHSet& l) : tl(l) { }
	const DirectoryListing::Directory::TTHSet& tl;
	bool operator()(const DirectoryListing::File::Ptr& i) const {
		return tl.count(i->getTTH()) > 0;
	}
};

struct DirectoryEmpty {
	bool operator()(const DirectoryListing::Directory::Ptr& aDir) const {
		bool r = aDir->getFileCount() + aDir->directories.size() == 0;
		return r;
	}
};

struct SizeLess {
	bool operator()(const DirectoryListing::File::Ptr& f) const {
		return f->getSize() < Util::convertSize(SETTING(SKIP_SUBTRACT), Util::KB);
	}
};

DirectoryListing::Directory::~Directory() {
	//for_each(files, DeleteFunction());
}

void DirectoryListing::Directory::clearAll() noexcept {
	//for_each(files, DeleteFunction());
	directories.clear();
	files.clear();
}

void DirectoryListing::Directory::filterList(DirectoryListing& dirList) noexcept {
	auto d = dirList.getRoot();

	TTHSet l;
	d->getHashList(l);
	filterList(l);
}

void DirectoryListing::Directory::filterList(DirectoryListing::Directory::TTHSet& l) noexcept {
	for(auto& d: directories) 
		d->filterList(l);

	directories.erase(remove_if(directories.begin(), directories.end(), DirectoryEmpty()), directories.end());
	files.erase(remove_if(files.begin(), files.end(), HashContained(l)), files.end());

	if((SETTING(SKIP_SUBTRACT) > 0) && (files.size() < 2)) {   //setting for only skip if folder filecount under x ?
		files.erase(remove_if(files.begin(), files.end(), SizeLess()), files.end());
	}
}

void DirectoryListing::Directory::getHashList(DirectoryListing::Directory::TTHSet& l) const noexcept {
	for(const auto& d: directories)  
		d->getHashList(l);

	for(const auto& f: files) 
		l.insert(f->getTTH());
}
	
void DirectoryListing::getLocalPaths(const File::Ptr& f, StringList& ret) const throw(ShareException) {
	if(f->getParent()->getAdls() && (f->getParent()->getParent() == root || !isOwnList))
		return;

	if (isOwnList) {
		string path;
		if (f->getParent()->getAdls())
			path = ((AdlDirectory*) f->getParent())->getFullPath();
		else
			path = f->getParent()->getPath();

		ShareManager::getInstance()->getRealPaths(Util::toAdcFile(path + f->getName()), ret, Util::toInt(fileName));
	} else {
		ret = AirUtil::getDupePaths(f->getDupe(), f->getTTH());
	}
}

void DirectoryListing::getLocalPaths(const Directory::Ptr& d, StringList& ret) const throw(ShareException) {
	if(d->getAdls() && (d->getParent() == root || !isOwnList))
		return;

	string path;
	if (d->getAdls())
		path = ((AdlDirectory*) d.get())->getFullPath();
	else
		path = d->getPath();

	if (isOwnList) {
		ShareManager::getInstance()->getRealPaths(Util::toAdcFile(path), ret, Util::toInt(fileName));
	} else {
		ret = ShareManager::getInstance()->getDirPaths(path);
	}
}

int64_t DirectoryListing::Directory::getTotalSize(bool countAdls) const noexcept {
	if(!isComplete())
		return partialSize;
	if(!countAdls && getAdls())
		return 0;
	
	auto x = getFilesSize();
	for(const auto& d: directories) {
		if(!countAdls && d->getAdls())
			continue;
		x += d->getTotalSize(getAdls());
	}
	return x;
}

size_t DirectoryListing::Directory::getTotalFileCount(bool countAdls) const noexcept {
	if(!countAdls && getAdls())
		return 0;

	auto x = getFileCount();
	for(const auto& d: directories) {
		if(!countAdls && d->getAdls())
			continue;
		x += d->getTotalFileCount(getAdls());
	}
	return x;
}

void DirectoryListing::Directory::clearAdls() noexcept {
	for(auto i = directories.begin(); i != directories.end();) {
		if((*i)->getAdls()) {
			i = directories.erase(i);
		} else {
			++i;
		}
	}
}

string DirectoryListing::Directory::getPath() const noexcept {
	//make sure to not try and get the name of the root dir
	if(parent) {
		return parent->getPath() + name + '\\';
	}

	// root
	return Util::emptyString;
}

void DirectoryListing::setActive() noexcept {
	fire(DirectoryListingListener::SetActive());
}

int64_t DirectoryListing::Directory::getFilesSize() const noexcept {
	int64_t x = 0;
	for(const auto& f: files) {
		x += f->getSize();
	}
	return x;
}

uint8_t DirectoryListing::Directory::checkShareDupes() noexcept {
	uint8_t result = DUPE_NONE;
	bool first = true;
	for(auto& d: directories) {
		result = d->checkShareDupes();
		if(dupe == DUPE_NONE && first)
			setDupe((DupeType)result);

		//full dupe with same type for non-dupe dir, change to partial (or pass partial dupes to upper level folder)
		else if (result == DUPE_SHARE && dupe == DUPE_NONE && !first)
			setDupe(DUPE_SHARE_PARTIAL);
		else if(result == DUPE_SHARE_PARTIAL && (dupe == DUPE_NONE || dupe == DUPE_SHARE) && !first)
			setDupe(DUPE_SHARE_PARTIAL);
		else if (result == DUPE_QUEUE && dupe == DUPE_NONE && !first)
			setDupe(DUPE_QUEUE_PARTIAL);
		else if( result == DUPE_QUEUE_PARTIAL && (dupe == DUPE_NONE || dupe == DUPE_QUEUE) && !first)
			setDupe(DUPE_QUEUE_PARTIAL);

		//change to mixed dupe type
		else if((dupe == DUPE_SHARE || dupe == DUPE_SHARE_PARTIAL) && (result == DUPE_QUEUE || result == DUPE_QUEUE_PARTIAL))
			setDupe(DUPE_SHARE_QUEUE);
		else if ((dupe == DUPE_QUEUE || dupe == DUPE_QUEUE_PARTIAL) && (result == DUPE_SHARE || result == DUPE_SHARE_PARTIAL))
			setDupe(DUPE_SHARE_QUEUE);

		else if (result == DUPE_SHARE_QUEUE)
			setDupe(DUPE_SHARE_QUEUE);

		first = false;
	}

	first = true;
	for(auto& f: files) {
		//don't count 0 byte files since it'll give lots of partial dupes
		//of no interest
		if(f->getSize() > 0) {			
			//if it's the first file in the dir and no sub-folders exist mark it as a dupe.
			if (dupe == DUPE_NONE && f->getDupe() == DUPE_SHARE && directories.empty() && first)
				setDupe(DUPE_SHARE);
			else if (dupe == DUPE_NONE && f->isQueued() && directories.empty() && first)
				setDupe(DUPE_QUEUE);

			//if it's the first file in the dir and we do have sub-folders but no dupes, mark as partial.
			else if (dupe == DUPE_NONE && f->getDupe() == DUPE_SHARE && !directories.empty() && first)
				setDupe(DUPE_SHARE_PARTIAL);
			else if (dupe == DUPE_NONE && f->isQueued() && !directories.empty() && first)
				setDupe(DUPE_QUEUE_PARTIAL);
			
			//if it's not the first file in the dir and we still don't have a dupe, mark it as partial.
			else if (dupe == DUPE_NONE && f->getDupe() == DUPE_SHARE && !first)
				setDupe(DUPE_SHARE_PARTIAL);
			else if (dupe == DUPE_NONE && f->isQueued() && !first)
				setDupe(DUPE_QUEUE_PARTIAL);
			
			//if it's a dupe and we find a non-dupe, mark as partial.
			else if (dupe == DUPE_SHARE && f->getDupe() != DUPE_SHARE)
				setDupe(DUPE_SHARE_PARTIAL);
			else if (dupe == DUPE_QUEUE && !f->isQueued())
				setDupe(DUPE_QUEUE_PARTIAL);

			//if we find different type of dupe, change to mixed
			else if ((dupe == DUPE_SHARE || dupe == DUPE_SHARE_PARTIAL) && f->isQueued())
				setDupe(DUPE_SHARE_QUEUE);
			else if ((dupe == DUPE_QUEUE || dupe == DUPE_QUEUE_PARTIAL) && f->getDupe() == DUPE_SHARE)
				setDupe(DUPE_SHARE_QUEUE);

			first = false;
		}
	}
	return dupe;
}

void DirectoryListing::checkShareDupes() noexcept {
	root->checkShareDupes();
	root->setDupe(DUPE_NONE); //never show the root as a dupe or partial dupe.
}

void DirectoryListing::addViewNfoTask(const string& aPath, bool aAllowQueueList, DupeOpenF aDupeF) noexcept {
	addAsyncTask([=] { findNfoImpl(aPath, aAllowQueueList, aDupeF); });
}

void DirectoryListing::addMatchADLTask() noexcept {
	addAsyncTask([=] { matchAdlImpl(); });
}

void DirectoryListing::addListDiffTask(const string& aFile, bool aOwnList) noexcept {
	addAsyncTask([=] { listDiffImpl(aFile, aOwnList); });
}

void DirectoryListing::addPartialListTask(const string& aXml, const string& aBase, bool reloadAll /*false*/, bool changeDir /*true*/, std::function<void()> f) noexcept {
	//onStateChanged();
	addAsyncTask([=] { loadPartialImpl(aXml, aBase, reloadAll, changeDir, f); });
}

void DirectoryListing::addFullListTask(const string& aDir) noexcept {
	//onStateChanged();
	addAsyncTask([=] { loadFileImpl(aDir); });
}

void DirectoryListing::addQueueMatchTask() noexcept {
	addAsyncTask([=] { matchQueueImpl(); });
}

void DirectoryListing::close() noexcept {
	closing = true;
	tasks.stop([=] {
		fire(DirectoryListingListener::Close());
	});
}

void DirectoryListing::addSearchTask(const string& aSearchString, int64_t aSize, int aTypeMode, int aSizeMode, const StringList& aExtList, const string& aDir) noexcept {
	addAsyncTask([=] { searchImpl(aSearchString, aSize, aTypeMode, aSizeMode, aExtList, aDir); });
}

void DirectoryListing::addAsyncTask(DispatcherQueue::Callback&& f) noexcept {
	if (isClientView) {
		tasks.addTask(move(f));
	} else {
		dispatch(f);
	}
}

void DirectoryListing::dispatch(DispatcherQueue::Callback& aCallback) noexcept {
	try {
		aCallback();
	} catch (const std::bad_alloc&) {
		LogManager::getInstance()->message(STRING_F(LIST_LOAD_FAILED, ClientManager::getInstance()->getNick(hintedUser.user, hintedUser.hint) % STRING(OUT_OF_MEMORY)), LogMessage::SEV_ERROR);
		fire(DirectoryListingListener::LoadingFailed(), "Out of memory");
	} catch (const AbortException&) {
		fire(DirectoryListingListener::LoadingFailed(), Util::emptyString);
	} catch(const ShareException& e) {
		fire(DirectoryListingListener::LoadingFailed(), e.getError());
	} catch (const QueueException& e) {
		fire(DirectoryListingListener::UpdateStatusMessage(), "Queueing failed:" + e.getError());
	} catch (const Exception& e) {
		LogManager::getInstance()->message(STRING_F(LIST_LOAD_FAILED, ClientManager::getInstance()->getNick(hintedUser.user, hintedUser.hint) % e.getError()), LogMessage::SEV_ERROR);
		fire(DirectoryListingListener::LoadingFailed(), ClientManager::getInstance()->getNick(hintedUser.user, hintedUser.hint) + ": " + e.getError());
	}
}

void DirectoryListing::listDiffImpl(const string& aFile, bool aOwnList) throw(Exception, AbortException) {
	int64_t start = GET_TICK();
	if (isOwnList && partialList) {
		// we need the recursive list for this
		loadShareDirectory(Util::emptyString, true);
		partialList = false;
	}

	DirectoryListing dirList(hintedUser, false, aFile, false, aOwnList);
	dirList.loadFile();

	root->filterList(dirList);
	fire(DirectoryListingListener::LoadingFinished(), start, Util::emptyString, false, true);
}

void DirectoryListing::matchAdlImpl() throw(AbortException) {
	int64_t start = GET_TICK();
	root->clearAdls(); //not much to check even if its the first time loaded without adls...
	ADLSearchManager::getInstance()->matchListing(*this);
	fire(DirectoryListingListener::LoadingFinished(), start, Util::emptyString, false, true);
}

void DirectoryListing::loadFileImpl(const string& aInitialDir) throw(Exception, AbortException) {
	int64_t start = GET_TICK();
	partialList = false;

	fire(DirectoryListingListener::LoadingStarted(), false);
	bool reloading = !root->directories.empty();

	if (reloading) {
		root->clearAll();
		baseDirs.clear();
	}

	loadFile();

	onLoadingFinished(start, aInitialDir, reloading, true);
}

void DirectoryListing::onLoadingFinished(int64_t aStartTime, const string& aDir, bool aReloadList, bool aChangeDir) noexcept {
	if (matchADL) {
		fire(DirectoryListingListener::UpdateStatusMessage(), CSTRING(MATCHING_ADL));
		ADLSearchManager::getInstance()->matchListing(*this);
	}

	if (!getIsOwnList() && SETTING(DUPES_IN_FILELIST) && isClientView)
		checkShareDupes();

	auto dir = findDirectory(aDir);
	if (dir) {
		dir->setLoading(false);
		updateCurrentLocation(dir);
		onStateChanged();
	}
	
	fire(DirectoryListingListener::LoadingFinished(), aStartTime, aDir, aReloadList, aChangeDir);
}

void DirectoryListing::updateCurrentLocation(const Directory::Ptr& aCurrentDirectory) noexcept {
	currentLocation.directories = aCurrentDirectory->directories.size();
	currentLocation.files = aCurrentDirectory->files.size();
	currentLocation.totalSize = aCurrentDirectory->getTotalSize(false);
	currentLocation.directory = aCurrentDirectory;
}

void DirectoryListing::searchImpl(const string& aSearchString, int64_t aSize, int aTypeMode, int aSizeMode, const StringList& aExtList, const string& aDir) noexcept {
	lastResult = GET_TICK();
	maxResultCount = 0;
	curResultCount = 0;
	searchResults.clear();

	fire(DirectoryListingListener::SearchStarted());

	curSearch.reset(SearchQuery::getSearch(aSearchString, Util::emptyString, aSize, aTypeMode, aSizeMode, aExtList, SearchQuery::MATCH_NAME, true, 100));
	if (isOwnList && partialList) {
		SearchResultList results;
		try {
			ShareManager::getInstance()->search(results, *curSearch, Util::toInt(fileName), CID(), aDir);
		} catch (...) {}

		for (const auto& sr : results)
			searchResults.insert(sr->getPath());

		curResultCount = searchResults.size();
		maxResultCount = searchResults.size();
		endSearch(false);
	} else if (partialList && !hintedUser.user->isNMDC()) {
		SearchManager::getInstance()->addListener(this);

		searchToken = Util::toString(Util::rand());
		ClientManager::getInstance()->directSearch(hintedUser, aSizeMode, aSize, aTypeMode, aSearchString, searchToken, aExtList, aDir, 0, SearchManager::DATE_DONTCARE);

		TimerManager::getInstance()->addListener(this);
	} else {
		const auto dir = (aDir.empty()) ? root : findDirectory(Util::toNmdcFile(aDir), root);
		if (dir)
			dir->search(searchResults, *curSearch);

		curResultCount = searchResults.size();
		maxResultCount = searchResults.size();
		endSearch(false);
	}
}

void DirectoryListing::loadPartialImpl(const string& aXml, const string& aBaseDir, bool reloadAll, bool changeDir, std::function<void()> completionF) throw(Exception, AbortException) {
	if (!partialList)
		return;

	auto baseDir = isOwnList && reloadAll ? "/" : Util::toAdcFile(aBaseDir);

	bool reloading = reloadAll;
	if (!reloading) {
		auto bd = baseDirs.find(Text::toLower(baseDir));
		if (bd != baseDirs.end()) {
			reloading = bd->second.second;
		}
	}

	if (reloading) {
		fire(DirectoryListingListener::LoadingStarted(), false);

		if (baseDir.empty() || reloadAll) {
			baseDirs.clear();
			root->clearAll();
			if (baseDir.empty())
				root->setComplete();
			else
				root->setType(Directory::TYPE_INCOMPLETE_CHILD);
		} else {
			auto cur = findDirectory(Util::toNmdcFile(baseDir));
			if (cur && (!cur->directories.empty() || !cur->files.empty())) {
				//we have been here already, just reload all items
				cur->clearAll();

				//also clean the visited dirs
				for (auto i = baseDirs.begin(); i != baseDirs.end();) {
					if (AirUtil::isSub(i->first, baseDir, '/')) {
						i = baseDirs.erase(i);
					}
					else {
						i++;
					}
				}
			}
		}
	}

	if (!reloading) {
		fire(DirectoryListingListener::LoadingStarted(), true);
	}

	int dirsLoaded = 0;
	if (isOwnList) {
		dirsLoaded = loadShareDirectory(Util::toNmdcFile(baseDir));
	} else {
		dirsLoaded = updateXML(aXml, baseDir);
	}

	onLoadingFinished(0, Util::toNmdcFile(baseDir), reloadAll || (reloading && baseDir == "/"), changeDir);

	if (completionF) {
		completionF();
	}
}

void DirectoryListing::matchQueueImpl() noexcept {
	int matches = 0, newFiles = 0;
	BundleList bundles;
	QueueManager::getInstance()->matchListing(*this, matches, newFiles, bundles);
	fire(DirectoryListingListener::QueueMatched(), AirUtil::formatMatchResults(matches, newFiles, bundles, false));
}

void DirectoryListing::on(ClientManagerListener::UserDisconnected, const UserPtr& aUser, bool /*wentOffline*/) noexcept {
	if (aUser != hintedUser.user) {
		return;
	}

	fire(DirectoryListingListener::UserUpdated());
}
void DirectoryListing::on(ClientManagerListener::UserUpdated, const OnlineUser& aUser) noexcept {
	if (aUser.getUser() != hintedUser.user) {
		return;
	}

	fire(DirectoryListingListener::UserUpdated());
}

void DirectoryListing::on(SearchManagerListener::SR, const SearchResultPtr& aSR) noexcept {
	if (compare(aSR->getToken(), searchToken) == 0) {
		lastResult = GET_TICK();

		string path;
		if (supportsASCH()) {
			path = aSR->getPath();
		} else {
			//convert the regular search results
			path = aSR->getType() == SearchResult::TYPE_DIRECTORY ? Util::getNmdcParentDir(aSR->getPath()) : aSR->getFilePath();
		}

		auto insert = searchResults.insert(path);
		if (insert.second)
			curResultCount++;

		if (maxResultCount == curResultCount)
			lastResult = 0; //we can call endSearch only from the TimerManagerListener thread
	}
}

void DirectoryListing::on(ClientManagerListener::DirectSearchEnd, const string& aToken, int aResultCount) noexcept {
	if (compare(aToken, searchToken) == 0) {
		maxResultCount = aResultCount;
		if (maxResultCount == curResultCount)
			endSearch(false);
	}
}

void DirectoryListing::on(TimerManagerListener::Second, uint64_t aTick) noexcept {
	if (curResultCount == 0) {
		if (lastResult + 5000 < aTick)
			endSearch(true);
	} else if (lastResult + 1000 < aTick) {
		endSearch(false);
	}
}

void DirectoryListing::endSearch(bool timedOut /*false*/) noexcept {
	SearchManager::getInstance()->removeListener(this);
	TimerManager::getInstance()->removeListener(this);

	if (curResultCount == 0) {
		curSearch = nullptr;
		fire(DirectoryListingListener::SearchFailed(), timedOut);
	} else {
		curResult = searchResults.begin();
		changeDirectory(*curResult, RELOAD_NONE, true);
	}
}

int DirectoryListing::loadShareDirectory(const string& aPath, bool aRecurse) throw(Exception, AbortException) {
	auto mis = ShareManager::getInstance()->generatePartialList(Util::toAdcFile(aPath), aRecurse, Util::toInt(fileName));
	if (mis) {
		return loadXML(*mis, true, Util::toAdcFile(aPath));
	}

	//might happen if have refreshed the share meanwhile
	throw Exception(CSTRING(FILE_NOT_AVAILABLE));
}

bool DirectoryListing::changeDirectory(const string& aPath, ReloadMode aReloadMode, bool aIsSearchChange) noexcept {
	const auto dir = aPath.empty() ? root : findDirectory(aPath, root);
	if (!dir) {
		return false;
	}

	if (!partialList || dir->getLoading() || (dir->isComplete() && aReloadMode == RELOAD_NONE)) {
		fire(DirectoryListingListener::ChangeDirectory(), aPath, aIsSearchChange);
	} else {
		try {
			if (isOwnList) {
				dir->setLoading(true);
				fire(DirectoryListingListener::ChangeDirectory(), aPath, aIsSearchChange);
				addPartialListTask(aPath, aPath, aReloadMode == RELOAD_ALL);
			} else if (getUser()->isOnline()) {
				dir->setLoading(true);
				fire(DirectoryListingListener::ChangeDirectory(), aPath, aIsSearchChange);
				QueueManager::getInstance()->addList(hintedUser, QueueItem::FLAG_PARTIAL_LIST | QueueItem::FLAG_CLIENT_VIEW, aPath);
			} else {
				fire(DirectoryListingListener::UpdateStatusMessage(), STRING(USER_OFFLINE));
			}
		} catch (const Exception& e) {
			fire(DirectoryListingListener::LoadingFailed(), e.getError());
		}
	}

	updateCurrentLocation(dir);
	return true;
}

bool DirectoryListing::nextResult(bool prev) noexcept {
	if (prev) {
		if (curResult == searchResults.begin()) {
			return false;
		}
		advance(curResult, -1);
	} else {
		if (static_cast<size_t>(distance(searchResults.begin(), curResult)) == searchResults.size()-1) {
			return false;
		}
		advance(curResult, 1);
	}

	changeDirectory(*curResult, RELOAD_NONE, true);
	return true;
}

bool DirectoryListing::isCurrentSearchPath(const string& path) const noexcept {
	if (searchResults.empty())
		return false;

	return *curResult == path;
}

void DirectoryListing::onListRemovedQueue(const string& aTarget, const string& aDir, bool aFinished) noexcept {
	if (!aFinished) {
		addAsyncTask([=] {
			auto dir = findDirectory(aDir);
			if (dir) {
				dir->setLoading(false);
				fire(DirectoryListingListener::RemovedQueue(), aDir);

				onStateChanged();
			}
		});
	}

	TrackableDownloadItem::onRemovedQueue(aTarget, aFinished);
}

void DirectoryListing::on(ShareManagerListener::DirectoriesRefreshed, uint8_t, const RefreshPathList& aPaths) noexcept{
	if (!partialList)
		return;

	string lastVirtual;
	for (const auto& p : aPaths) {
		auto vPath = ShareManager::getInstance()->realToVirtual(p, Util::toInt(fileName));
		if (!vPath.empty() && lastVirtual != vPath && findDirectory(vPath)) {
			addAsyncTask([=] { loadPartialImpl(Util::emptyString, vPath, false, false, nullptr); });
			lastVirtual = vPath;
		}
	}
}

} // namespace dcpp
