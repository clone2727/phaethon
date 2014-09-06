/* Phaethon - A FLOSS resource explorer for BioWare's Aurora engine games
 *
 * Phaethon is the legal property of its developers, whose names
 * can be found in the AUTHORS file distributed with this source
 * distribution.
 *
 * Phaethon is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * Phaethon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Phaethon. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file mainwindow.h
 *  Phaethon's main window.
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <map>

#include <wx/wx.h>
#include <wx/treectrl.h>

#include <wx/generic/stattextg.h>

#include "common/ustring.h"
#include "common/filetree.h"

#include "aurora/types.h"
#include "aurora/archive.h"

namespace Aurora {
	class KEYFile;
	class KEYDataFile;
}

class ResourceTreeItem : public wxTreeItemData {
public:
	enum Source {
		kSourceDirectory   = 0,
		kSourceFile        = 1,
		kSourceArchiveFile = 2
	};

	ResourceTreeItem(const Common::FileTree::Entry &entry);
	ResourceTreeItem(Aurora::Archive *archive, const Aurora::Archive::Resource &resource);
	~ResourceTreeItem();

	const Common::UString &getName() const;
	uint32 getSize() const;
	Source getSource() const;

	Aurora::FileType getFileType() const;
	Aurora::ResourceType getResourceType() const;

private:
	struct Data {
		boost::filesystem::path path;

		Aurora::Archive *archive;
		bool addedArchiveMembers;
		uint32 archiveIndex;
	};

	Common::UString _name;
	uint32 _size;
	Source _source;
	Data _data;

	Data &getData();

	friend class ResourceTree;
};

class MainWindow;

class ResourceTree : public wxTreeCtrl {
wxDECLARE_DYNAMIC_CLASS(ResourceTree);

public:
	ResourceTree();
	ResourceTree(wxWindow *parent, MainWindow &mainWindow);
	~ResourceTree();

	wxTreeItemId addRoot(ResourceTreeItem *item);
	wxTreeItemId appendItem(wxTreeItemId parent, ResourceTreeItem *item);

	int OnCompareItems(const wxTreeItemId &item1, const wxTreeItemId &item2);

	void onSelChanged(wxTreeEvent &event);
	void onItemExpanding(wxTreeEvent &event);

private:
	enum Image {
		kImageNone = -1,
		kImageDir  =  0,
		kImageFile     ,
		kImageMAX
	};

	MainWindow *_mainWindow;

	static Image getImage(const ResourceTreeItem &item);

	void forceArchiveChildren(const ResourceTreeItem &item, wxTreeItemId id);

	wxDECLARE_EVENT_TABLE();
};

class MainWindow : public wxFrame {
public:
	MainWindow(const wxString &title, const wxPoint &pos, const wxSize &size);
	~MainWindow();

	bool open(Common::UString path);

	void resourceTreeSelect(const ResourceTreeItem *item);

	Aurora::Archive *getArchive(const boost::filesystem::path &path);

private:
	typedef std::map<Common::UString, Aurora::Archive *> ArchiveMap;
	typedef std::map<Common::UString, Aurora::KEYDataFile *> KEYDataFileMap;

	Common::UString _path;
	Common::FileTree _files;

	ResourceTree *_resourceTree;

	wxGenericStaticText *_resInfoName;
	wxGenericStaticText *_resInfoSize;
	wxGenericStaticText *_resInfoFileType;
	wxGenericStaticText *_resInfoResType;

	ArchiveMap _archives;
	KEYDataFileMap _keyDataFiles;

	void onOpenDir(wxCommandEvent &event);
	void onOpenFile(wxCommandEvent &event);
	void onClose(wxCommandEvent &event);
	void onQuit(wxCommandEvent &event);
	void onAbout(wxCommandEvent &event);

	void forceRedraw();

	void close();
	void populateTree();
	void populateTree(const Common::FileTree::Entry &e, wxTreeItemId t);

	void loadKEYDataFiles(Aurora::KEYFile &key);
	Aurora::KEYDataFile *getKEYDataFile(const Common::UString &file);

	wxDECLARE_EVENT_TABLE();
};

#endif // MAINWINDOW_H
