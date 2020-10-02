//==================================================================//
/*
	Untrunc - mp4.cpp

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio
																	*/
//==================================================================//

#include <cassert>
#include <vector>
#include <algorithm>
#include <functional>
#include <string>
#include <iostream>
#include <ios>          // Pre-C++11: may not be included by <iostream>.
#include <iomanip>
#include <limits>

#ifndef  __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS    1
#endif
#ifndef  __STDC_CONSTANT_MACROS
# define __STDC_CONSTANT_MACROS 1
#endif
extern "C" {
#include <stdint.h>
#ifdef _WIN32
# include <io.h>        // for: _isatty()
#else
# include <unistd.h>    // for: isatty()
#endif
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/log.h"
}  // extern "C"

#include "mp4.h"
#include "atom.h"
#include "file.h"
#include "log.h"

// Stdio file descriptors.
#ifndef STDIN_FILENO
# define STDIN_FILENO   0
# define STDOUT_FILENO  1
# define STDERR_FILENO  2
#endif


#include <algorithm>
using namespace std;


namespace {
const int MaxFrameLength = 20000000;


// Store start-up addresses of C++ stdio stream buffers as identifiers.
// These addresses differ per process and must be statically linked in.
// Assume that the stream buffers at these stored addresses
//  are always connected to their underlaying stdio files.
static const streambuf* const StdioBufs[] = {
	cin.rdbuf(), cout.rdbuf(), cerr.rdbuf(), clog.rdbuf()
};
// Store start-up terminal/TTY statuses of C++ stdio stream buffers.
// These statuses differ per process and must be statically linked in.
// Assume that the statuses don't change during the process life-time.
static const bool StdioTtys[sizeof(StdioBufs)/sizeof(StdioBufs[0])] = {
		#ifdef _WIN32
		_isatty(STDIN_FILENO), _isatty(STDOUT_FILENO), _isatty(STDERR_FILENO), _isatty(STDERR_FILENO)
		#else
		(bool)isatty(STDIN_FILENO),  (bool)isatty(STDOUT_FILENO),  (bool)isatty(STDERR_FILENO),  (bool)isatty(STDERR_FILENO)
		#endif
};

// Is a Terminal/Console/TTY connected to the C++ stream?
// Use on C++ stdio chararacter streams: cin, cout, cerr and clog.
bool isATerminal(const ios& strm) {
	for(unsigned int i = 0; i < sizeof(StdioBufs)/sizeof(StdioBufs[0]); ++i) {
		if(strm.rdbuf() == StdioBufs[i])
			return StdioTtys[i];
	}
	return false;
}

// Configure FFmpeg/Libav logging for use in C++.
class AvLog {
	int lvl;
#ifdef AV_LOG_PRINT_LEVEL
	int flgs;
#endif
public:
#ifdef AV_LOG_PRINT_LEVEL
# define DEFAULT_AVLOG_FLAGS	AV_LOG_PRINT_LEVEL
#else
# define DEFAULT_AVLOG_FLAGS	0
#endif

	explicit AvLog()
		: lvl(av_log_get_level())
	#ifdef AV_LOG_PRINT_LEVEL
		, flgs(av_log_get_flags())
	#endif
	{
		av_log_set_flags(DEFAULT_AVLOG_FLAGS);
		cout.flush();   // Flush C++ standard streams.
		//cerr.flush();   // Unbuffered -> nothing to flush.
		clog.flush();
	}

	explicit AvLog(int level, int flags = DEFAULT_AVLOG_FLAGS)
		: lvl(av_log_get_level())
	#ifdef AV_LOG_PRINT_LEVEL
		, flgs(av_log_get_flags())
	#endif
	{
		if(lvl < level)
			av_log_set_level(level);
		av_log_set_flags(flags);
		cout.flush();   // Flush C++ standard streams.
		//cerr.flush();   // Unbuffered -> nothing to flush.
		clog.flush();
	}

	~AvLog() {
		fflush(stdout); // Flush C stdio files.
		fflush(stderr);
		av_log_set_level(lvl);
#ifdef AV_LOG_PRINT_LEVEL
		av_log_set_flags(flgs);
#endif
	}
};

// Redirect C files.
// This does not effect C++ standard I/O streams (cin, cout, cerr, clog).
class FileRedirect {
	FILE *&file_ref;
	FILE * file_value;
public:
	explicit FileRedirect(FILE *&file, FILE *to_file)
		: file_ref(file)
		, file_value(file)
	{
		file = to_file;
		if(file_ref)  fflush(file_ref);
	}

	~FileRedirect() {
		if(file_ref)  fflush(file_ref);
		file_ref = file_value;
	}
};
}; // namespace



// Mp4
Mp4::Mp4() : timescale(0), duration(0), root(NULL), context(NULL) { }

Mp4::~Mp4() {
	close();
}

void Mp4::open(string filename) {
	Log::debug << "Opening: " << filename << '\n';
	close();

	try {  // Parse ok file.
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		root = new Atom;
		do {
			Atom *atom = new Atom;
			atom->parse(file);
			Log::debug << "Found atom: " << atom->name << '\n';

			root->children.push_back(atom);
		} while(!file.atEnd());
	} catch(const string &error) {
		Log::info << error << "\n";
		Log::flush();
		throw string("Failed parsing working mp4. Maybe the broken and working files got inverted.");
	}/* catch(...) {
		throw string("Failed parsing working mp4. Maybe the broken and working files got inverted.");
	}*/
	// {
	file_name = filename;

	if(root->atomByName("ctts"))
		Log::debug << "Found 'Composition Time To Sample' atom (ctts). Out of order samples possible.\n";

	if(root->atomByName("sdtp"))
		Log::debug << "Found 'Independent and Disposable Samples' atom (sdtp). I and P frames might need to recover that info.\n";

	Atom *mvhd = root->atomByName("mvhd");
	if(!mvhd)
		throw string("Missing 'Movie Header' atom (mvhd)");
	// ASSUME: mvhd atom version 0.
	timescale = mvhd->readInt(12);
	duration  = mvhd->readInt(16);

	{  // Setup AV library.
		AvLog useAvLog();
		// Register all formats and codecs.
		av_register_all();
		// Open video file.
#ifdef OLD_AVFORMAT_API
		int error = av_open_input_file(&context, filename.c_str(), NULL, 0, NULL);
#else
		int error = avformat_open_input(&context, filename.c_str(), NULL, NULL);
#endif
		if(error != 0)
			throw "Could not parse AV file: " + filename;

		// Retrieve stream information.
#ifdef OLD_AVFORMAT_API
		if(av_find_stream_info(context) < 0)
#else
		if(avformat_find_stream_info(context, NULL) < 0)
#endif
			throw string("Could not find stream info");
	}  // {

	parseTracks();
}

void Mp4::close() {
	Atom *rm_root = root;
	root      = NULL;   // Invalidate Mp4 data.
	timescale = 0;
	duration  = 0;
	tracks.clear();     // Must clear tracks before closing context.
	if(context) {
		AvLog useAvLog(AV_LOG_ERROR);
#ifdef OLD_AVFORMAT_API
		av_close_input_file(&context);
#else
		avformat_close_input(&context);
#endif
		context = NULL;
	}
	file_name.clear();
	delete rm_root;
}

void Mp4::printMediaInfo() {
	if(context) {
		cout.flush();
		clog.flush();
		Log::info << "Media Info:\n"
				  << "  Default stream: " << av_find_default_stream_index(context) << '\n';
		AvLog useAvLog(AV_LOG_INFO);
		FileRedirect redirect(stderr, stdout);
		av_dump_format(context, 0, file_name.c_str(), 0);
	}
}

void Mp4::printAtoms() {
	if(root) {
		Log::info << "Atoms:\n";
		root->print(0);
	}
}

bool Mp4::makeStreamable(string filename, string output_filename) {
	Log::info << "Make Streamable: " << filename << '\n';
	Atom atom_root;
	{  // Parse input file.
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		while(!file.atEnd()) {
			Atom *atom = new Atom;
			atom->parse(file);
			Log::debug << "Found atom: " << atom->name << '\n';
			atom_root.children.push_back(atom);
		}
	}  // {

	Atom *ftyp = atom_root.atomByName("ftyp");
	Atom *moov = atom_root.atomByName("moov");
	Atom *mdat = atom_root.atomByName("mdat");
	if(!moov || !mdat) {
		if(!moov)
			Log::error << "Missing 'Container for all the Meta-data' atom (moov).\n";
		if(!mdat)
			Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return false;
	}

	if(mdat->start > moov->start) {
		Log::info << "File is already streamable." << endl;
		return true;
	}

	int64_t old_start = mdat->start  + 8;
	int64_t new_start = moov->length + 8;
	if(ftyp)
		new_start += ftyp->length;

	int64_t diff = new_start - old_start;
	Log::debug << "Old: " << old_start << " -> New: " << new_start << '\n';

#if 0 // MIGHT HAVE TO FIX THIS ONE TOO?
	Atom *co64 = trak->atomByName("co64");
	if(co64) {
		trak->prune("co64");
		Atom *stbl = trak->atomByName("stbl");
		if(stbl) {
			Atom *new_stco = new Atom;
			memcpy(new_stco->name, "stco", min(sizeof("stco"), sizeof(new_stco->name)-1));
			stbl->children.push_back(new_stco);
		}
	}
#endif
	std::vector<Atom *> stcos = moov->atomsByName("stco");
	for(unsigned int i = 0; i < stcos.size(); ++i) {
		Atom *stco = stcos[i];
		int32_t nchunks = stco->readInt(4); // 4 version, 4 number of entries, 4 entries.
		for(int j = 0; j < nchunks; ++j) {
			int64_t pos    = int64_t(8) + 4*j;
			int64_t offset = stco->readInt(pos) + diff;
			Log::debug << "O: " << offset << '\n';
			stco->writeInt(offset, pos);
		}
	}

	{  // Save to output file.
		Log::debug << "Saving to: " << output_filename << '\n';
		File file;
		if(!file.create(output_filename))
			throw "Could not create file for writing: " + output_filename;

		if(ftyp)
			ftyp->write(file);
		moov->write(file);
		mdat->write(file);
	}  // {
	Log::debug << endl;
	return true;
}

bool Mp4::save(string output_filename) {
	// We save all atoms except:
	//  ctts: composition offset (we use sample to time).
	//  cslg: because it is used only when ctts is present.
	//  stps: partial sync, same as sync.
	//
	// Movie is made by ftyp, moov, mdat (we need to know mdat begin, for absolute offsets).
	// Assume offsets in stco are absolute and so to find the relative just subtrack mdat->start + 8.

	Log::info << "Saving to: " << output_filename << '\n';
	if(!root) {
		Log::error << "No file opened.\n";
		return false;
	}

	if(timescale == 0) {
		timescale = 600;  // Default movie time scale.
		Log::info << "Using new movie time scale: " << timescale << ".\n";
	}
	duration = 0;
	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Track &track = tracks[i];
		Log::debug << "Track " << i << " (" << track.codec.name << "): duration: "
				   << track.duration << " timescale: " << track.timescale << '\n';
		if(track.timescale == 0 && track.duration != 0)
			Log::info << "Track " << i << " (" << track.codec.name << ") has no time scale.\n";

		track.writeToAtoms();

		// Convert duration to movie timescale.
		if(timescale == 0) continue;  // Shouldn't happen.
		// Use default movie time scale if no track time scale was found.
		int track_timescale = (track.timescale != 0) ? track.timescale : 600;
		//int track_duration  = static_cast<int>(double(track.duration) * double(timescale) / double(track_timescale));
		//convert track duration (in track.timescale units) to movie timesscale units.
		int track_duration  = static_cast<int>((int64_t(track.duration) * timescale - 1 + track_timescale)
											   / track_timescale);

		if(duration < track_duration)
			duration = track_duration;

		Atom *tkhd = track.trak->atomByName("tkhd");
		if(!tkhd) {
			Log::debug << "Missing 'Track Header' atom (tkhd).\n";
			continue;
		}
		if(tkhd->readInt(20) == track_duration) continue;

		Log::debug << "Adjusting track duration to movie timescale: New duration: "
				   << track_duration << " timescale: " << timescale << ".\n";
		tkhd->writeInt(track_duration, 20); // In movie timescale, not track timescale.
	}

	Log::debug << "Movie duration: " << duration/(double)timescale << "s with timescale: " << timescale << '\n';
	Atom *mvhd = root->atomByName("mvhd");
	if(!mvhd)
		throw string("Missing 'Movie Header' atom (mvhd)");
	mvhd->writeInt(duration, 16);

	Atom *ftyp = root->atomByName("ftyp");
	Atom *moov = root->atomByName("moov");
	Atom *mdat = root->atomByName("mdat");
	if(!moov || !mdat) {
		if(!moov)
			Log::error << "Missing 'Container for all the Meta-data' atom (moov).\n";
		if(!mdat)
			Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return false;
	}

	moov->prune("ctts");
	moov->prune("cslg");
	moov->prune("stps");

	root->updateLength();





	//we need to add mdat header bytes
	int64_t offset = moov->length + 8;
	if(mdat->length64)
		offset += 8;

	if(ftyp)
		offset += ftyp->length; // Not all .mov have an ftyp.

	for(unsigned int t = 0; t < tracks.size(); ++t) {
		Track &track = tracks[t];
		for(unsigned int i = 0; i < track.offsets.size(); ++i)
			track.offsets[i] += offset;

		track.writeToAtoms();  // Need to save the offsets back to the atoms.
	}

	{  // Save to output file.
		File file;
		if(!file.create(output_filename))
			throw "Could not create file for writing: " + output_filename;

		if(ftyp)
			ftyp->write(file);
		moov->write(file);
		mdat->write(file);
	}  // {
	return true;
}

void Mp4::analyze(int analyze_track, bool interactive) {
	Log::info << "Analyze:\n";
	if(!root) {
		Log::error << "No file opened.\n";
		return;
	}

	Atom *mdat = root->atomByName("mdat");
	if(!mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	if(interactive) {
		// For interactive analyzis, std::cin & std::cout must be connected to a terminal/tty.
		if(!isATerminal(cin)) {
			Log::debug << "Cannot analyze interactively as input doesn't come directly from a terminal.\n";
			interactive = false;
		}
		if(interactive && !isATerminal(cout)) {
			Log::debug << "Cannot analyze interactively as output doesn't go directly to a terminal.\n";
			interactive = false;
		}
		if(interactive)
			cin.clear();  // Reset state - clear transient errors of previous input operations.
#ifdef VERBOSE1
		clog.flush();
#endif
	}

	for(unsigned int i = 0; i < tracks.size(); ++i) {
		if(analyze_track != -1 && i != analyze_track)
			continue;
		Log::info << "\n\nTrack " << i << endl;
		Track &track = tracks[i];

		if(track.hint_track) {
			Log::info << "Hint track for track: " << track.hinted_id << "\n";
		} else {
			Log::info << "Track codec: " << track.codec.name << '\n';
		}

		Log::info << "Keyframes  : " << track.keyframes.size() << "\n\n";

		if(track.codec.pcm) {
			Log::info << "PCM codec, skipping keyframes\n\n";
		} else if(track.default_size) {
			Log::info << "Default size packets, skipping keyframes\n\n";
		} else {
			for(unsigned int i = 0; i < track.keyframes.size(); ++i) {
				int k = track.keyframes[i];
				int64_t  offset = track.offsets[k] - mdat->content_start;
				uint32_t begin  = mdat->readInt(offset);
				uint32_t next   = mdat->readInt(offset + 4);
				Log::debug << setw(8) << k
						   << " Size: " << setw(6) << track.getSize(k)
						   << " offset " << setw(10) << track.offsets[k]
							  << "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next << dec << '\n';
			}
		}

		if(track.default_size) {
			Log::info << "Constant size for packet: " << track.default_size << "\n";
		} else {
			Log::info << "Sizes for packets: " << "\n";
			for(int i = 0; i < 10 && i < track.sizes.size(); i++) {
				Log::info << track.sizes[i] << " ";
			}
			Log::info << "\n";
		}

		if(track.default_time) {
			Log::info << "Constant time for packet: " << track.default_time << endl;
		}

		if(track.default_size) {
			if(!track.codec.pcm) {
				Log::info << "Not a PCM codec, default size though..  we have hope.\n";

			}
			for(Track::Chunk &chunk: track.chunks) {
				int64_t offset = chunk.offset - mdat->content_start;

				int64_t maxlength64 = mdat->contentSize() - offset;
				if(maxlength64 > MaxFrameLength)
					maxlength64 = MaxFrameLength;
				int maxlength = static_cast<int>(maxlength64);

				int32_t begin = mdat->readInt(offset);
				int32_t next  = mdat->readInt(offset + 4);
				int32_t end   = mdat->readInt(offset + track.getSize(i) - 4);

				Log::info << " Size: " << setw(6) << chunk.size
						  << " offset " << setw(10) << chunk.offset
						  << "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next
						  << " end: " << setw(8) << end << dec << '\n';
			}
		}



		for(unsigned int i = 0; i < track.offsets.size(); ++i) {

			int64_t offset = track.offsets[i] - mdat->content_start;
			unsigned char *start = &(mdat->content[offset]);
			int64_t maxlength64 = mdat->contentSize() - offset;
			if(maxlength64 > MaxFrameLength)
				maxlength64 = MaxFrameLength;
			int maxlength = static_cast<int>(maxlength64);

			int32_t begin = mdat->readInt(offset);
			int32_t next  = mdat->readInt(offset + 4);
			Log::info << " Size: " << setw(6) << track.getSize(i)
					  << " offset " << setw(10) << track.offsets[i]
						 << "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next
						 <<  dec << '\n';

			Log::flush();
			Match match = track.codec.match(start, maxlength);
			if(match.length == track.sizes[i])
				continue;

			if(match.length == 0) {
				Log::error << "- Match failed!\n";
			} else if(match.length < 0 || match.length > MaxFrameLength) {
				Log::error << "- Invalid length!\n";
			} else {
				Log::error << "- Length mismatch!\n";
			}

			Log::info << "Match: length " << match.length << "\n";
			if(interactive) {
				Log::info << "  <Press [Enter] for next match>\r";
				cin.ignore(numeric_limits<streamsize>::max(), '\n');
			}
		}
	}
}

void Mp4::simulate() {

	//TODO remove duplicated code with analyze.
	Log::info << "Simulate:\n";

	Log::info << "Analyze:\n";
	if(!root) {
		Log::error << "No file opened.\n";
		return;
	}
	Atom *original_mdat = root->atomByName("mdat");
	if(!original_mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	BufferedAtom *mdat = findMdat(file_name);
	if(!mdat) {
		Log::error << "MDAT not found.\n";
		return;
	}


	//sort packets by start, length, track id

	std::vector<Match> packets;

	std::string codecs[tracks.size()];

	for(unsigned int t = 0; t < tracks.size(); ++t) {
		Track &track = tracks[t];
		codecs[t] = track.codec.name;
		//if pcm -> offsets should be chunks!

		if(track.default_size) {
			Log::debug << "Track " << t << " packets: " << track.chunks.size() << endl;
			for(unsigned int i = 0;i < track.chunks.size(); i++) {
				Track::Chunk &chunk = track.chunks[i];
				Match match;
				match.id = t;

				match.offset = chunk.offset;
				match.length = chunk.size;
				match.duration = track.default_time? track.default_time : track.times[i];
				packets.push_back(match);
			}
		} else {
			Log::debug << "Track " << t << " packets: " << track.offsets.size() << endl;

			for(unsigned int i = 0; i < track.offsets.size(); ++i) {
				Match match;

				match.id = t;
				match.offset = track.offsets[i];
				match.length = track.sizes[i];
				match.duration = track.default_time? track.default_time : track.times[i];
				packets.push_back(match);
			}
		}
	}
	std::sort(packets.begin(), packets.end(), [](const Match &m1, const Match &m2) { return m1.offset < m2.offset; });

	if(packets[0].offset != original_mdat->content_start) {
		Log::error << "First packet does not start with mdat, finding the start of the packets might be problematic" << endl;
	}


	//ensure mdat is correctly found.
	if(original_mdat->content_start != mdat->file_begin) {
		Log::error << "Wrong start of mdat: " << mdat->file_begin << " should be " << original_mdat->content_start << "\n";
		mdat->file_begin = mdat->content_start = packets[0].offset;
	}



	int64_t offset = 0; //mdat->file_begin;
	for(Match &m: packets) {
		if(m.offset != mdat->file_begin + offset) {
			Log::error << "Some empty space to be skipped! Real start = " << m.offset << " mdat start guessed at: " << offset + mdat->file_begin << "\n";
			break;
		}

		unsigned char *start = mdat->getFragment(offset, 8);
		unsigned int begin = readBE<int>(start);
		unsigned int next  = readBE<int>(start + 4);
		Log::debug << "\n" << codecs[m.id] << " offset: " << setw(10) << (m.offset) << " Length: " << m.length
					<< "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next << dec << "\n";

		MatchGroup matches = match(offset, mdat);
		Match &best = matches[0];

		for(Match m: matches) {
			Log::debug << "Match for: " << m.id << " (" << codecs[m.id] << ") chances: " << m.chances << " length: " << m.length << "\n";
		}
		if(best.chances == 0.0f) {
			//we could not detect best, in reconstruction we need to backtrack
			Log::error << "Could not match packet for track " << m.id << "\n";
			Log::flush();

			Log::info << "  <Press [Enter] for next match>\r";
			cin.ignore(numeric_limits<streamsize>::max(), '\n');

//			break;
		}
		if(m.id  != best.id) {
			Log::error << "Mismatch! Packet track should be on track: " << m.id << " (" << codecs[m.id] << ") it is: " << best.id << " (" << codecs[best.id] << ")\n";
			Log::flush();

			Log::info << "  <Press [Enter] for next match>\r";
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
//			break;
		}
		if(m.length != best.length) {
			Log::error << "Packet length is wrong." << endl;
			Log::flush();

			Log::info << "  <Press [Enter] for next match>\r";
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
//			break;
		}
		offset += m.length;
	}
}

MatchGroup Mp4::match(int64_t offset, BufferedAtom *mdat) {
	MatchGroup group;
	group.offset = offset;

	int64_t maxlength64 = mdat->contentSize() - offset;
	if(maxlength64 > MaxFrameLength)
		maxlength64 = MaxFrameLength;
	unsigned char *start = mdat->getFragment(offset, maxlength64);
	int maxlength = static_cast<int>(maxlength64);


	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Track &track = tracks[i];
		Match m = track.codec.match(start, maxlength);
		m.id = i;
		m.offset = group.offset;
		group.push_back(m);
	}

	sort(group.begin(), group.end(), [](const Match &m1, const Match &m2) { return m1.chances > m2.chances; });
	return group;
}

void Mp4::writeTracksToAtoms() {
	for(unsigned int i = 0; i < tracks.size(); ++i)
		tracks[i].writeToAtoms();
}

bool Mp4::parseTracks() {
	assert(root != NULL);

	Atom *mdat = root->atomByName("mdat");
	if(!mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return false;
	}
	vector<Atom *> traks = root->atomsByName("trak");
	for(unsigned int i = 0; i < traks.size(); ++i) {
		Track track;
		track.codec.context = context->streams[i]->codec;
		track.parse(traks[i], mdat);
		track.codec.stats.init(track, mdat);

		tracks.push_back(track);
	}
	return true;
}

BufferedAtom *Mp4::findMdat(std::string filename, bool same_mdat_start, bool ignore_mdat_start) {
	BufferedAtom *mdat = new BufferedAtom(filename);
	int64_t start = findMdat(mdat->file, ignore_mdat_start);
	if(start < 0) {
		delete mdat;
		return nullptr;
	}

	mdat->start = 0; //will be overwritten in repair.
	memcpy(mdat->name, "mdat", 5);

	mdat->content_start = start;
	mdat->file_begin = start;
	mdat->file_end = mdat->file.length();
	return mdat;
}

/* strategy:
 *
 * 1) Look for mdat. It's almost always a good start, but sometime, the actual packets can start from 8 to 200000k and more after.
 *     1.a) if non start guessable packets with fixed lenght are present we are blindly looking for them
 *     2.b) if we have guessable start make a map of possible starts and check with what we have found, in that case we need to lookup find mdat start with a different approach.
 * 2) if method 1 fails, we need to look for start guessable packets.
 *
 * Note: avc1 has size then start and for keyframes they are pretty guessable.
 */

int64_t Mp4::contentStart() {
	vector<int64_t> offsets;
	for(Track &track: tracks) {
		for(Track::Chunk &chunk: track.chunks) {
			offsets.push_back(chunk.offset);
			break;
		}
	}
	sort(offsets.begin(), offsets.end());
	return offsets[0];
}

int64_t Mp4::findMdat(File &file, bool same_mdat_start, bool ignore_mdat_start) {

	if(same_mdat_start)
		return contentStart();

	//look for mdat
	int64_t mdat_offset = -1;
	char m[4];
	m[3] = 0;
	if(!ignore_mdat_start) {
		for(uint64_t i = 0; i < file.size()-4 && i < 20000000; i++) {
			char c;
			file.readChar(&c, 1);
			if(c != 'm') continue;
			uint64_t pos = file.pos();
			file.readChar(m, 3);
			if(strcmp(m, "dat") != 0) {
				file.seek(pos);
				continue;
			}
			//first is not good enough sometimes it's the second.
			//but not the last either, sometimes it is at the end.
			mdat_offset = file.pos();
			break;
		}
	}
	if(mdat_offset == -1) {
		//TODO if we have some unique beginnigs, try to spot the first one.

		for(uint64_t i = 0; i < file.size()-4 && i < 20000000; i++) {
			file.seek(i);
			int32_t begin32 = file.readInt();

			if(begin32 == 0) continue;
			bool found = false;
			for(Track &track: tracks) {
				if(track.codec.stats.beginnings32.count(begin32)) {
					found = true;
					break;
				}
			}
			if(found)
				return i;
		}
	}

	//wild guess with little hope.
	if(mdat_offset == -1) {
		uint32_t threshold = 0x00030000;
		cerr << "Mdat not found!" << endl;
		//look for low number.
		for(uint64_t i = 0; i < file.size()-4; i++) {
			file.seek(i);
			uint32_t s = file.readUInt();
			if(s > threshold) continue;
			//let's hope that skipping foward that number we get another low number.
			file.seek(i + s + 4);
			s = file.readUInt();
			if(s > threshold) continue;

			mdat_offset = i;
			break;
		}
	}
	return mdat_offset;
}


//skip vast tract of zeros (up to the last one.
//multiple of 1024. If it's all zeros skip.
//if it's less than 8 bytes dont (might be alac?)
//otherwise we need to search for the actual begin using matches.
int zeroskip(BufferedAtom *mdat, unsigned char *start, int64_t maxlength) {
	int64_t block_size = std::min(int64_t(1<<10), maxlength);

	//skip 4 bytes at a time.
	int k = 0;
	for(;k < block_size - 4; k+= 4) {
		int value = readBE<int>(start + k);
		if(value != 0)
			break;
	}

	//don't skip very short zero sequences
	if(k < 12)
		return 0;

	//play conservative of non aligned zero blocks
	if(k < block_size)
		k -= 4;

	//zero bytes block aligned.

	Log::debug << "Skipping zero bytes: " << k << "\n";
	return k;
}

int Mp4::searchNext(BufferedAtom *mdat, int64_t offset) {
	int64_t maxlength64 = mdat->contentSize() - offset;
	if(maxlength64 > MaxFrameLength)
		maxlength64 = MaxFrameLength;
	unsigned char *start = mdat->getFragment(offset, maxlength64);
	int maxlength = static_cast<int>(maxlength64);
	Match best;
	best.chances = 0;
	best.offset = 0;
	for(Track &track: tracks) {
		Match m = track.codec.search(start, maxlength);
		if(m.chances != 0 && best.chances == 0 || m.offset < best.offset)
			best = m;
	}
	return best.offset;
}

bool Mp4::repair(string corrupt_filename, bool same_mdat_start, bool ignore_mdat_start) {
	Log::info << "Repair: " << corrupt_filename << '\n';
	BufferedAtom *mdat = NULL;
	File file;
	if(!file.open(corrupt_filename))
		throw "Could not open file: " + corrupt_filename;

	if(0) {  // Parse corrupt file.


		// Find mdat.  This fails with krois and a few other.
		// TODO: Check for multiple mdat, or just look for the first one.
		while(true) {
			Atom atom;
			try {
				atom.parseHeader(file);
			} catch(string) {
				throw string("Failed to parse atoms in truncated file");
			}

			if(atom.name != string("mdat")) {
				off_t pos = file.pos();
				file.seek(pos - 8 + atom.length);
				continue;
			}

			mdat = new BufferedAtom(corrupt_filename);
			mdat->start = atom.start;
			memcpy(mdat->name, atom.name, sizeof(mdat->name)-1);
			memcpy(mdat->head, atom.head, sizeof(mdat->head));
			memcpy(mdat->version, atom.version, sizeof(mdat->version));

			mdat->file_begin = file.pos();
			mdat->file_end   = file.length() - file.pos();

			Log::debug << "MDAT SIZE: " << mdat->file_end - mdat->file_begin << endl;			//mdat->content = file.read(file.length() - file.pos());
			break;
		}
	} else {
		int64_t mdat_offset= findMdat(file, same_mdat_start, ignore_mdat_start);

		if(mdat_offset < 0) {
			Log::error << "Failed finding start" << endl;
			return false;
		}

		mdat = new BufferedAtom(corrupt_filename);
		mdat->start = mdat_offset;
		memcpy(mdat->name, "mdat", 5);
		//	memcpy(mdat->head, atom.head, sizeof(mdat->head));
		//memcpy(mdat->version, atom.version, sizeof(mdat->version));gedit
		file.seek(mdat_offset);
		mdat->file_begin = file.pos();
		mdat->file_end   = file.length();
	}

	Log::debug << "Mdat start: " << mdat->file_begin << " end: " << mdat->file_end << endl;
	for(unsigned int i = 0; i < tracks.size(); ++i)
		tracks[i].clear();


	// mp4a can be decoded and reports the number of samples (duration in samplerate scale).
	// In some videos the duration (stts) can be variable and we can rebuild them using these values.
	vector<int> audiotimes;
	//TODO: if it fails, try again with the same offset as the good one.
	//carefu the offset is relative to mdat, use the same absolute position.
	int64_t offset = 0;



	std::vector<MatchGroup> matches;

	//keep track of how many backtraced.
	int backtracked = 0;
	while(offset <  mdat->contentSize()) {
		int64_t maxlength64 = mdat->contentSize() - offset;
		if(maxlength64 > MaxFrameLength)
			maxlength64 = MaxFrameLength;
		unsigned char *start = mdat->getFragment(offset, maxlength64);
		int maxlength = static_cast<int>(maxlength64);



		unsigned int begin = readBE<int>(start); //mdat->readInt(offset);
		unsigned int next  = readBE<int>(start + 4);//mdat->readInt(offset + 4);

		Log::debug << "Offset: " << setw(10) << (mdat->file_begin + offset)
					   << "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next << dec << '\n';
		cout.flush();


		if(begin == 0) {
			int skipped = zeroskip(mdat, start, maxlength64);
			if(skipped) {
				Log::debug << "Skipping " << skipped << " zeroes!" << endl;
				offset += skipped;
				continue;
			}
		}



		Log::flush();
		//skip internal atoms
		if(!strncmp("moov", (char *)start + 4, 4) ||
			!strncmp("free", (char *)start + 4, 4) ||
			!strncmp("hoov", (char *)start + 4, 4) ||
			!strncmp("moof", (char *)start + 4, 4) ||
			!strncmp("wide", (char *)start + 4, 4)) {

			Log::debug << "Skipping containers for all the meta-data atom (moov, free, wide): begin: 0x"
					   << hex << begin << dec << ".\n";
			offset += begin;
			continue;
		}

		if(!strncmp("mdat", (char *)start + 4, 4)) {
			Log::debug << "Mdat encoutered, skipping header\n";
			offset += 8;
			continue;
		}
		//new strategy: try to match all tracks.
		//each codec can:
		//matchable match with some probability.
		//know length

		//if more than one non matchable and not searchable we die.


		//if we have >1 possible match try best prob.
		//if no match assume is a non matchable for wich we know length.
		//search for a beginning
		//backtrace otherwise

		MatchGroup group;
		group.offset = offset;
		for(unsigned int i = 0; i < tracks.size(); ++i) {
			Track &track = tracks[i];

			Match m = track.codec.match(start, maxlength);
			m.id = i;
			group.push_back(m);
			//if(track.codec.pcm)
			//	step = track.codec.pcm_bytes_per_sample;
		}
		sort(group.begin(), group.end());

		Match &best = group.back();
		//no hope!
		if(best.chances == 0.0f) {

			Log::flush();
			//can we backtrack? if not, we are done


			//we are at the end!
			if(mdat->contentSize() -offset < 65000) {
				Log::debug << "We are at the end of the file: " << offset << endl;
				break;
			}
			//look for a different candidate in the past matches
			while(backtracked < 7) {

				if(matches.size() == 0) {
					backtracked = 7;
					break;
				}

				backtracked++;

				MatchGroup &last = matches.back();
				//last packet found in previous group wasn't good.
				last.pop_back();

				if(last.size() == 0) {
					//we need to go to the previous group
					matches.pop_back();
					continue;
				}
				Match &candidate = last.back();
				if(candidate.chances > 0.0f && candidate.length > 0 ) {
					offset = last.offset + candidate.length;
					break;
				}
				//no luck either, try another one looping
			}
			if(backtracked >= 7) {
				Log::error << "Backtracked enough!" << endl;
				break;
			}

			//we changed the offset lets restart.
			continue;
		}
		backtracked = 0;

		if(mdat->file_begin + offset + best.length > mdat->file_end)
			break;

		if(best.length) {
			Log::debug << "Matched track: " << best.id << " length: " << best.length << "\n";
			offset += best.length;

		} else {
			Log::debug << "Unknown length, search for next beginning" << endl;
			best.length = searchNext(mdat, offset);

			if(!best.length) {
				Log::error << "Could not guess length of best match" << endl;
				//throw "Can't guess the length of any packet.";
				break;
			}
			offset += best.length;
			//This should only happen with pcm codecs, and here we should search for the beginning of another codec
		}
		matches.push_back(group);
	}


	//copy matches into tracks
	int count = 0;
	for(MatchGroup &group: matches) {
		assert(group.size() > 0);
		Match &match = group.back();

		Track &track = tracks[match.id];
		if(match.keyframe)
			track.keyframes.push_back(track.offsets.size());

		track.offsets.push_back(group.offset);
		if(track.default_size) {
			//if number of samples per chunk is variable, encode each sample in a different chunk.
			if(track.default_chunk_nsamples == 0)
				track.nsamples += 1;
			else
				track.nsamples += track.default_chunk_nsamples;
		} else {
			//TODO: this won't work for things with samples per chunk != 1;
			track.nsamples++;
			track.sizes.push_back(match.length);
		}

		if(match.duration)
			audiotimes.push_back(match.duration);
	}

	mdat->file_end = mdat->file_begin + offset;
	mdat->length   = mdat->file_end - mdat->file_begin;

	Log::info << "Found " << matches.size() << " packets.\n";

	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Log::info << "Found " << tracks[i].offsets.size() << " chunks for " << tracks[i].codec.name << endl;
		if(audiotimes.size() == tracks[i].offsets.size())
			swap(audiotimes, tracks[i].times);

		tracks[i].fixTimes();
	}

	Atom *original_mdat = root->atomByName("mdat");
	if(!original_mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		delete mdat;
		return false;
	}
	mdat->start = original_mdat->start;
	Log::debug << "Replacing 'Media Data content' atom (mdat).\n";

	root->replace(original_mdat, mdat);
	//original_mdat->content.swap(mdat->content);
	//original_mdat->start = -8;
	delete original_mdat;

	return true;
}

// vim:set ts=4 sw=4 sts=4 noet:
