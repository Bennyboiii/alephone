/*
 *  FileHandler_SDL.cpp - Platform-independant file handling, SDL implementation
 *
 *  Written in 2000 by Christian Bauer
 */

#include "cseries.h"
#include "FileHandler.h"
#include "resource_manager.h"

#include "shell.h"
#include "interface.h"
#include "game_errors.h"

#include "sdl_dialogs.h"
#include "sdl_widgets.h"

#include <stdio.h>
#include <errno.h>
#include <string>

#include <SDL_endian.h>

#ifdef HAVE_UNISTD_H
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#endif


// From shell_sdl.cpp
extern FileSpecifier global_data_dir, local_data_dir, saved_games_dir, recordings_dir;


/*
 *  Utility functions
 */

bool is_applesingle(SDL_RWops *f, bool rsrc_fork, long &offset, long &length)
{
	// Check header
	SDL_RWseek(f, 0, SEEK_SET);
	uint32 id = SDL_ReadBE32(f);
	uint32 version = SDL_ReadBE32(f);
	if (id != 0x00051600 || version != 0x00020000)
		return false;

	// Find fork
	uint32 req_id = rsrc_fork ? 2 : 1;
	SDL_RWseek(f, 0x18, SEEK_SET);
	int num_entries = SDL_ReadBE16(f);
	while (num_entries--) {
		uint32 id = SDL_ReadBE32(f);
		int32 ofs = SDL_ReadBE32(f);
		int32 len = SDL_ReadBE32(f);
		//printf(" entry id %d, offset %d, length %d\n", id, ofs, len);
		if (id == req_id) {
			offset = ofs;
			length = len;
			return true;
		}
	}
	return false;
}

bool is_macbinary(SDL_RWops *f, long &data_length, long &rsrc_length)
{
	// This only recognizes MacBinary II files
	SDL_RWseek(f, 0, SEEK_SET);
	uint8 header[128];
	SDL_RWread(f, header, 1, 128);
	if (header[0] || header[1] > 63 || header[74] || header[122] < 0x81 || header[123] < 0x81)
		return false;

	// Check CRC
	uint16 crc = 0;
	for (int i=0; i<124; i++) {
		uint16 data = header[i] << 8;
		for (int j=0; j<8; j++) {
			if ((data ^ crc) & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
			data <<= 1;
		}
	}
	//printf("crc %02x\n", crc);
	if (crc != ((header[124] << 8) | header[125]))
		return false;

	// CRC valid, extract fork sizes
	data_length = (header[83] << 24) | (header[84] << 16) | (header[85] << 8) | header[86];
	rsrc_length = (header[87] << 24) | (header[88] << 16) | (header[89] << 8) | header[90];
	return true;
}


/*
 *  Opened file
 */

OpenedFile::OpenedFile() : f(NULL), err(0), is_forked(false), fork_offset(0), fork_length(0) {}

bool OpenedFile::IsOpen()
{
	return f != NULL;
}

bool OpenedFile::Close()
{
	if (f) {
		SDL_RWclose(f);
		f = NULL;
		err = 0;
	}
	return true;
}

bool OpenedFile::GetPosition(long &Position)
{
	if (f == NULL)
		return false;

	err = 0;
	Position = SDL_RWtell(f) - fork_offset;
	return true;
}

bool OpenedFile::SetPosition(long Position)
{
	if (f == NULL)
		return false;

	err = 0;
	if (SDL_RWseek(f, Position + fork_offset, SEEK_SET) < 0)
		err = errno;
	return err == 0;
}

bool OpenedFile::GetLength(long &Length)
{
	if (f == NULL)
		return false;

	if (is_forked)
		Length = fork_length;
	else {
		long pos = SDL_RWtell(f);
		SDL_RWseek(f, 0, SEEK_END);
		Length = SDL_RWtell(f);
		SDL_RWseek(f, pos, SEEK_SET);
	}
	err = 0;
	return true;
}

bool OpenedFile::Read(long Count, void *Buffer)
{
	if (f == NULL)
		return false;

	err = 0;
	if (SDL_RWread(f, Buffer, 1, Count) != Count)
		err = errno;
	return err == 0;
}

bool OpenedFile::Write(long Count, void *Buffer)
{
	if (f == NULL)
		return false;

	err = 0;
	if (SDL_RWwrite(f, Buffer, 1, Count) != Count)
		err = errno;
	return err == 0;
}


/*
 *  Loaded resource
 */

LoadedResource::LoadedResource() : p(NULL), size(0) {}

bool LoadedResource::IsLoaded()
{
	return p != NULL;
}

void LoadedResource::Unload()
{
	if (p) {
		free(p);
		p = NULL;
		size = 0;
	}
}

size_t LoadedResource::GetLength()
{
	return size;
}

void *LoadedResource::GetPointer(bool DoDetach)
{
	void *ret = p;
	if (DoDetach)
		Detach();
	return ret;
}

void LoadedResource::Detach()
{
	p = NULL;
	size = 0;
}


/*
 *  Opened resource file
 */

OpenedResourceFile::OpenedResourceFile() : f(NULL), saved_f(NULL), err(0) {}

bool OpenedResourceFile::Push()
{
	saved_f = cur_res_file();
	if (saved_f != f)
		use_res_file(f);
	err = 0;
	return true;
}

bool OpenedResourceFile::Pop()
{
	if (f != saved_f)
		use_res_file(saved_f);
	err = 0;
	return true;
}

bool OpenedResourceFile::Check(uint32 Type, int16 ID)
{
	Push();
	bool result = has_1_resource(Type, ID);
	err = result ? 0 : errno;
	Pop();
	return result;
}

bool OpenedResourceFile::Get(uint32 Type, int16 ID, LoadedResource &Rsrc)
{
	Push();
	bool success = get_1_resource(Type, ID, Rsrc);
	err = success ? 0 : errno;
	Pop();
	return success;
}

bool OpenedResourceFile::IsOpen()
{
	return f != NULL;
}

bool OpenedResourceFile::Close()
{
	if (f) {
		close_res_file(f);
		f = NULL;
		err = 0;
	}
	return true;
}


/*
 *  File specification
 */

const FileSpecifier &FileSpecifier::operator=(const FileSpecifier &other)
{
	if (this != &other) {
		name = other.name;
		err = other.err;
	}
	return *this;
}

// Get last element of path
void FileSpecifier::GetName(char *part) const
{
#if defined(__unix__) || defined(__BEOS__)

	string::size_type pos = name.rfind('/');
	if (pos == string::npos)
		strcpy(part, name.c_str());
	else
		strcpy(part, name.substr(pos + 1).c_str());

#else
#error FileSpecifier::GetLastPart() not implemented for this platform
#endif
}

// Create file
bool FileSpecifier::Create(int Type)
{
	Delete();
	// files are automatically created when opened for writing
	err = 0;
	return true;
}

// Create directory
bool FileSpecifier::CreateDirectory()
{
#if defined(__unix__) || defined(__BEOS__)

	err = 0;
	if (mkdir(name.c_str(), 0777) < 0)
		err = errno;
	return err == 0;

#else
#error FileSpecifier::CreateDirectory() not implemented for this platform
#endif
}

// Open data file
bool FileSpecifier::Open(OpenedFile &OFile, bool Writable)
{
	OFile.Close();
	OFile.is_forked = false;
	OFile.fork_offset = 0;
	OFile.fork_length = 0;
	SDL_RWops *f = OFile.f = SDL_RWFromFile(name.c_str(), Writable ? "wb" : "rb");
	err = f ? 0 : errno;
	if (f == NULL) {
		set_game_error(systemError, err);
		return false;
	}
	if (Writable)
		return true;

	// Transparently handle AppleSingle and MacBinary II files on reading
	long offset, data_length, rsrc_length;
	if (is_applesingle(f, false, offset, data_length)) {
		OFile.is_forked = true;
		OFile.fork_offset = offset;
		OFile.fork_length = data_length;
		SDL_RWseek(f, offset, SEEK_SET);
		return true;
	} else if (is_macbinary(f, data_length, rsrc_length)) {
		OFile.is_forked = true;
		OFile.fork_offset = 128;
		OFile.fork_length = data_length;
		SDL_RWseek(f, 128, SEEK_SET);
		return true;
	}
	SDL_RWseek(f, 0, SEEK_SET);
	return true;
}

// Open resource file
bool FileSpecifier::Open(OpenedResourceFile &OFile, bool Writable)
{
	OFile.Close();
	OFile.f = open_res_file(*this);
	err = OFile.f ? 0 : errno;
	if (OFile.f == NULL) {
		set_game_error(systemError, err);
		return false;
	} else
		return true;
}

// Check for existence of file
bool FileSpecifier::Exists()
{
#if defined(__unix__) || defined(__BEOS__)

	// Check whether the file is readable
	err = 0;
	if (access(name.c_str(), R_OK) < 0)
		err = errno;
	return err == 0;

#else
#error FileSpecifier::Exists() not implemented for this platform
#endif
}

// Get modification date
TimeType FileSpecifier::GetDate()
{
#if defined(__unix__) || defined(__BEOS__)

	struct stat st;
	err = 0;
	if (stat(name.c_str(), &st) < 0) {
		err = errno;
		return 0;
	}
	return st.st_mtime;

#else
#error FileSpecifier::GetDate() not implemented for this platform
#endif
}

// Determine file type
int FileSpecifier::GetType()
{
	// Open file
	OpenedFile f;
	if (!Open(f))
		return NONE;
	SDL_RWops *p = f.GetRWops();
	long file_length = 0;
	f.GetLength(file_length);

	// Check for Sounds file
	{
		f.SetPosition(0);
		uint32 version = SDL_ReadBE32(p);
		uint32 tag = SDL_ReadBE32(p);
		if (version == 1 && tag == FOUR_CHARS_TO_INT('s', 'n', 'd', '2'))
			return _typecode_sounds;
	}

	// Check for Map/Physics file
	{
		f.SetPosition(0);
		int version = SDL_ReadBE16(p);
		int data_version = SDL_ReadBE16(p);
		if ((version == 0 || version == 1 || version == 2 || version == 4) && (data_version == 0 || data_version == 1 || data_version == 2)) {
			SDL_RWseek(p, 68, SEEK_CUR);
			int32 directory_offset = SDL_ReadBE32(p);
			if (directory_offset >= file_length)
				goto not_map;
			f.SetPosition(128);
			uint32 tag = SDL_ReadBE32(p);
			if (tag == FOUR_CHARS_TO_INT('L', 'I', 'N', 'S') || tag == FOUR_CHARS_TO_INT('P', 'N', 'T', 'S'))
				return _typecode_scenario;
			if (tag == FOUR_CHARS_TO_INT('M', 'N', 'p', 'x'))
				return _typecode_physics;
		}
not_map: ;
	}

	// Check for Shapes file
	{
		f.SetPosition(0);
		for (int i=0; i<32; i++) {
			uint32 status_flags = SDL_ReadBE32(p);
			int32 offset = SDL_ReadBE32(p);
			int32 length = SDL_ReadBE32(p);
			int32 offset16 = SDL_ReadBE32(p);
			int32 length16 = SDL_ReadBE32(p);
			if (status_flags != 0
			 || (offset != NONE && (offset >= file_length || offset + length > file_length))
			 || (offset16 != NONE && (offset16 >= file_length || offset16 + length16 > file_length)))
				goto not_shapes;
			SDL_RWseek(p, 12, SEEK_CUR);
		}
		return _typecode_shapes;
not_shapes: ;
	}

	// Not identified
	return NONE;
}

// Get free space on disk
bool FileSpecifier::GetFreeSpace(unsigned long &FreeSpace)
{
	// This is impossible to do in a platform-independant way, so we
	// just return 16MB which should be enough for everything
	FreeSpace = 16 * 1024 * 1024;
	err = 0;
	return true;
}

// Delete file
bool FileSpecifier::Delete()
{
	err = 0;
	if (remove(name.c_str()) < 0)
		err = errno;
	return err == 0;
}

// Set to local (per-user) data directory
void FileSpecifier::SetToLocalDataDir()
{
	name = local_data_dir.name;
}

// Set to saved games directory
void FileSpecifier::SetToSavedGamesDir()
{
	name = saved_games_dir.name;
}

// Set to recordings directory
void FileSpecifier::SetToRecordingsDir()
{
	name = recordings_dir.name;
}

// Set to global data directory
void FileSpecifier::SetToGlobalDataDir()
{
	name = global_data_dir.name;
}

// Add part to path name
void FileSpecifier::AddPart(const string &part)
{
#if defined(__unix__) || defined(__BEOS__)

	if (name.length() && name[name.length() - 1] == '/')
		name += part;
	else
		name = name + "/" + part;

#else
#error FileSpecifier::AddPart() not implemented for this platform
#endif
}

// Read directory contents
bool FileSpecifier::ReadDirectory(vector<dir_entry> &vec)
{
#if defined(__unix__) || defined(__BEOS__)

	vec.clear();

	DIR *d = opendir(name.c_str());
	if (d == NULL) {
		err = errno;
		return false;
	}
	struct dirent *de = readdir(d);
	while (de) {
		if (de->d_name[0] != '.' || (de->d_name[1] && de->d_name[1] != '.')) {
			FileSpecifier full_path = name;
			full_path.AddPart(de->d_name);
			struct stat st;
			if (stat(full_path.name.c_str(), &st) == 0)
				vec.push_back(dir_entry(de->d_name, st.st_size, S_ISDIR(st.st_mode), false));
		}
		de = readdir(d);
	}
	closedir(d);
	err = 0;
	return true;

#else
#error FileSpecifier::ReadDirectory() not implemented for this platform
#endif
}

// Copy file contents
bool FileSpecifier::CopyContents(FileSpecifier &source_name)
{
	err = 0;
	OpenedFile src, dst;
	if (source_name.Open(src)) {
		Delete();
		if (Open(dst, true)) {
			const int BUFFER_SIZE = 1024;
			uint8 buffer[BUFFER_SIZE];

			long length = 0;
			src.GetLength(length);

			while (length && err == 0) {
				long count = length > BUFFER_SIZE ? BUFFER_SIZE : length;
				if (src.Read(count, buffer)) {
					if (!dst.Write(count, buffer))
						err = dst.GetError();
				} else
					err = src.GetError();
				length -= count;
			}
		}
	} else
		err = source_name.GetError();
	if (err)
		Delete();
	return err == 0;
}
