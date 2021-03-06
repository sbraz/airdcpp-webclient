/*
* Copyright (C) 2011-2015 AirDC++ Project
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
#include "ViewFile.h"

#include "AirUtil.h"
#include "File.h"

namespace dcpp {
	ViewFile::ViewFile(const string& aTarget, const TTHValue& aTTH, bool aIsText, UpdateF&& aUpdateFunction) noexcept :
		path(aTarget), tth(aTTH), updateFunction(aUpdateFunction), text(aIsText) {

		onAddedQueue(path);
	}

	ViewFile::~ViewFile() noexcept {
		File::deleteFile(path);
	}

	string ViewFile::getDisplayName() const noexcept {
		return AirUtil::fromOpenFileName(Util::getFileName(path));
	}

	void ViewFile::onStateChanged() noexcept {
		updateFunction(tth);
	}

} //dcpp