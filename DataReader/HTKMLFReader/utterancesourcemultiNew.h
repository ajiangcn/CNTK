//
// <copyright file="utterancesourcemultiNew.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// utterancesourcemultiNew.h -- implementation of utterancesource.h that supports multiple feature and label sets
//

#pragma once

#include "Basics.h"                  // for attempt()
#include "htkfeatio.h"                  // for htkmlfreader
#include "latticearchive.h"             // for reading HTK phoneme lattices (MMI training)
#include "minibatchsourcehelpers.h"
#include "minibatchiterator.h"
#include "biggrowablevectors.h"
#include "ssematrix.h"
#include "unordered_set"

namespace msra { namespace dbn {

// ---------------------------------------------------------------------------
// minibatchutterancesource -- feature source to provide randomized utterances
// This also implements a frame-wise mode, which is layered on top of the utterance-wise mode
// and thus benefits from its goodies such as corpus-wide high-level randomization and chunk paging.
// ---------------------------------------------------------------------------
class minibatchutterancesourcemulti : public minibatchsource
{
    void operator=(const minibatchutterancesourcemulti & other); // non-assignable
    std::vector<size_t> vdim;                    // feature dimension after augmenting neighhors
    std::vector<size_t> leftcontext;             // number of frames to the left of the target frame in the context window
    std::vector<size_t> rightcontext;            // number of frames to the right of the target frame in the context window
    std::vector<unsigned int> sampperiod;        // (for reference and to check against model)
    std::vector<string> featkind;
    std::vector<size_t> featdim;
    const bool framemode;           // true -> actually return frame-level randomized frames (not possible in lattice mode)
    std::vector<std::vector<size_t>> counts;     // [s] occurrence count for all states (used for priors)
    int verbosity;
    // lattice reader
    //const std::vector<unique_ptr<latticesource>> &lattices;
    const latticesource & lattices;

    //std::vector<latticesource> lattices;
    // word-level transcripts (for MMI mode when adding best path to lattices)
    const map<wstring,msra::lattices::lattice::htkmlfwordsequence> & allwordtranscripts; // (used for getting word-level transcripts)
    //std::vector<map<wstring,msra::lattices::lattice::htkmlfwordsequence>> allwordtranscripts;
    // data store (incl. paging in/out of features and lattices)
    struct utterancedesc            // data descriptor for one utterance
    {
        msra::asr::htkfeatreader::parsedpath parsedpath;    // archive filename and frame range in that file
        size_t classidsbegin;       // index into allclassids[] array (first frame)

        utterancedesc (msra::asr::htkfeatreader::parsedpath&& ppath, size_t classidsbegin) : parsedpath (std::move(ppath)), classidsbegin (classidsbegin) {}

        const wstring & logicalpath() const { return parsedpath; /*type cast will return logical path*/ }
        size_t numframes() const { return parsedpath.numframes(); }
        wstring key() const                           // key used for looking up lattice (not stored to save space)
        {
#ifdef _MSC_VER
            static const wstring emptywstring;
            static const wregex deleteextensionre (L"\\.[^\\.\\\\/:]*$");
            return regex_replace (logicalpath(), deleteextensionre, emptywstring);  // delete extension (or not if none)
#else
            return removeExtension(logicalpath());
#endif
        }
    };

    // Make sure type 'utterancedesc' has a move constructor
    static_assert(std::is_move_constructible<utterancedesc>::value, "Type 'utterancedesc' should be move constructible!");

    struct utterancechunkdata       // data for a chunk of utterances
    {
        std::vector<utterancedesc> utteranceset;    // utterances in this set
        size_t numutterances() const { return utteranceset.size(); }

        std::vector<size_t> firstframes;    // [utteranceindex] first frame for given utterance
        mutable msra::dbn::matrix frames;   // stores all frames consecutively (mutable since this is a cache)
        size_t totalframes;         // total #frames for all utterances in this chunk
        mutable std::vector<shared_ptr<const latticesource::latticepair>> lattices;   // (may be empty if none)

        // construction
        utterancechunkdata() : totalframes (0) {}
        void push_back (utterancedesc &&/*destructive*/ utt)
        {
            if (isinram())
                LogicError("utterancechunkdata: frames already paged into RAM--too late to add data");
            firstframes.push_back (totalframes);
            totalframes += utt.numframes();
            utteranceset.push_back (std::move(utt));
        }

        // accessors to an utterance's data
        size_t numframes (size_t i) const { return utteranceset[i].numframes(); }
        size_t getclassidsbegin (size_t i) const { return utteranceset[i].classidsbegin; }
        msra::dbn::matrixstripe getutteranceframes (size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                LogicError("getutteranceframes: called when data have not been paged in");
            const size_t ts = firstframes[i];
            const size_t n = numframes(i);
            return msra::dbn::matrixstripe (frames, ts, n);
        }
        shared_ptr<const latticesource::latticepair> getutterancelattice (size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                LogicError("getutterancelattice: called when data have not been paged in");
            return lattices[i];
        }

        // paging
        // test if data is in memory at the moment
        bool isinram() const { return !frames.empty(); }
        // page in data for this chunk
        // We pass in the feature info variables by ref which will be filled lazily upon first read
        void requiredata (string & featkind, size_t & featdim, unsigned int & sampperiod, const latticesource & latticesource, int verbosity=0) const
        {
            if (numutterances() == 0)
                LogicError("requiredata: cannot page in virgin block");
            if (isinram())
                LogicError("requiredata: called when data is already in memory");
            try             // this function supports retrying since we read from the unrealible network, i.e. do not return in a broken state
            {
                msra::asr::htkfeatreader reader;    // feature reader (we reinstantiate it for each block, i.e. we reopen the file actually)
                // if this is the first feature read ever, we explicitly open the first file to get the information such as feature dimension
                if (featdim == 0)
                {
                    reader.getinfo (utteranceset[0].parsedpath, featkind, featdim, sampperiod);
                    fprintf (stderr, "requiredata: determined feature kind as %d-dimensional '%s' with frame shift %.1f ms\n", (int)featdim, featkind.c_str(), sampperiod / 1e4);
                }
                // read all utterances; if they are in the same archive, htkfeatreader will be efficient in not closing the file
                frames.resize (featdim, totalframes);
                if (!latticesource.empty())
                    lattices.resize (utteranceset.size());
                foreach_index (i, utteranceset)
                {
                    //fprintf (stderr, ".");
                    // read features for this file
                    auto uttframes = getutteranceframes (i);    // matrix stripe for this utterance (currently unfilled)
                    reader.read (utteranceset[i].parsedpath, (const string &) featkind, sampperiod, uttframes);  // note: file info here used for checkuing only
                    // page in lattice data
                    if (!latticesource.empty())
                        latticesource.getlattices (utteranceset[i].key(), lattices[i], uttframes.cols());
                }
                //fprintf (stderr, "\n");
                if (verbosity)
                    fprintf (stderr, "requiredata: %d utterances read\n", (int)utteranceset.size());
            }
            catch (...)
            {
                releasedata();
                throw;
            }
        }
        // page out data for this chunk
        void releasedata() const
        {
            if (numutterances() == 0)
                LogicError("releasedata: cannot page out virgin block");
            if (!isinram())
                LogicError("releasedata: called when data is not memory");
            // release frames
            frames.resize (0, 0);
            // release lattice data
            lattices.clear();
        }
    };
    std::vector<std::vector<utterancechunkdata>> allchunks;          // set of utterances organized in chunks, referred to by an iterator (not an index)
    std::vector<unique_ptr<biggrowablevector<CLASSIDTYPE>>> classids;            // [classidsbegin+t] concatenation of all state sequences
    std::vector<unique_ptr<biggrowablevector<HMMIDTYPE>>> phoneboundaries;
    bool issupervised() const { return !classids.empty(); }

    size_t numutterances;           // total number of utterances
    size_t _totalframes;            // total frames (same as classids.size() if we have labels)
    double timegetbatch;            // [v-hansu] for time measurement
    size_t chunksinram;             // (for diagnostics messages)

    class randomizer
    {
        int verbosity;
        bool framemode;
        size_t _totalframes;
        size_t numutterances;
        size_t randomizationrange;// parameter remembered; this is the full window (e.g. 48 hours), not the half window

        size_t currentsweep;            // randomization is currently cached for this sweep; if it changes, rebuild all below
        struct chunk                    // chunk as used in actual processing order (randomized sequence)
        {
            // the underlying chunk (as a non-indexed reference into the chunk set)
            std::vector<utterancechunkdata>::const_iterator uttchunkdata;
            const utterancechunkdata & getchunkdata() const { return *uttchunkdata; }
            size_t numutterances() const { return uttchunkdata->numutterances(); }
            size_t numframes() const { return uttchunkdata->totalframes; }

            // position in utterance-position space
            size_t utteranceposbegin;
            size_t utteranceposend() const { return utteranceposbegin + numutterances(); }

            // position on global time line
            size_t globalts;            // start frame on global timeline (after randomization)
            size_t globalte() const { return globalts + numframes(); }

            // randomization range limits
            // TODO only need to maintain for first feature stream
            size_t windowbegin;         // randomizedchunk index of earliest chunk that utterances in here can be randomized with
            size_t windowend;           // and end index [windowbegin, windowend)
            chunk(std::vector<utterancechunkdata>::const_iterator uttchunkdata, size_t utteranceposbegin, size_t globalts) : uttchunkdata(uttchunkdata), utteranceposbegin(utteranceposbegin), globalts(globalts) {}
        };
        std::vector<std::vector<chunk>> randomizedchunks;  // utterance chunks after being brought into random order (we randomize within a rolling window over them)

    public:
        struct sequenceref              // described a sequence to be randomized (in frame mode, a single frame; a full utterance otherwise)
        {
            size_t chunkindex;          // lives in this chunk (index into randomizedchunks[])
            size_t utteranceindex;      // utterance index in that chunk
            size_t numframes;           // (cached since we cannot directly access the underlying data from here)
            size_t globalts;            // start frame in global space after randomization (for mapping frame index to utterance position)
            size_t frameindex;          // 0 for utterances

            // TODO globalts - sweep cheaper?
            size_t globalte() const { return globalts + numframes; }            // end frame

            sequenceref()
                : chunkindex (0)
                , utteranceindex (0)
                , frameindex (0)
                , globalts (SIZE_MAX)
                , numframes (0) {}
            sequenceref (size_t chunkindex, size_t utteranceindex, size_t frameindex = 0)
                : chunkindex (chunkindex)
                , utteranceindex (utteranceindex)
                , frameindex (frameindex)
                , globalts (SIZE_MAX)
                , numframes (0) {}

            // TODO globalts and numframes only set after swapping, wouldn't need to swap them
            // TODO old frameref was more tighly packed (less fields, smaller frameindex and utteranceindex). We need to bring these space optimizations back.
        };

    private:
        std::vector<sequenceref> randomizedutterancerefs;          // [pos] randomized utterance ids
        std::unordered_map<size_t, size_t> randomizedutteranceposmap;     // [globalts] -> pos lookup table

        struct positionchunkwindow       // chunk window required in memory when at a certain position, for controlling paging
        {
            std::vector<chunk>::iterator definingchunk;       // the chunk in randomizedchunks[] that defined the utterance position of this utterance
            size_t windowbegin() const { return definingchunk->windowbegin; }
            size_t windowend() const { return definingchunk->windowend; }
            bool isvalidforthisposition (const sequenceref & sequence) const
            {
                return sequence.chunkindex >= windowbegin() && sequence.chunkindex < windowend(); // check if 'sequence' lives in is in allowed range for this position
                // TODO by construction sequences cannot span chunks (check again)
            }

            positionchunkwindow (std::vector<chunk>::iterator definingchunk) : definingchunk (definingchunk) {}
        };
        std::vector<positionchunkwindow> positionchunkwindows;      // [utterance position] -> [windowbegin, windowend) for controlling paging

        // shuffle a vector into random order by randomly swapping elements
        template<typename VECTOR> static void randomshuffle (VECTOR & v, size_t randomseed)
        {
            if (v.size() > RAND_MAX * (size_t) RAND_MAX)
                RuntimeError("randomshuffle: too large set: need to change to different random generator!");
            srand ((unsigned int) randomseed);
            foreach_index (i, v)
            {
                // pick a random location
                const size_t irand = msra::dbn::rand (0, v.size());

                // swap element i with it
                if (irand == (size_t) i)
                    continue;
                ::swap (v[i], v[irand]);
            }
        }

    public:
        // big long helper to update all cached randomization information
        // This is a rather complex process since we randomize on two levels:
        //  - chunks of consecutive data in the feature archive
        //  - within a range of chunks that is paged into RAM
        //     - utterances (in utt mode), or
        //     - frames (in frame mode)
        // The 'globalts' parameter is the start time that triggered the rerandomization; it is NOT the base time of the randomized area.
        size_t lazyrandomization (const size_t globalts,
            const std::vector<std::vector<utterancechunkdata>> & allchunks       // set of utterances organized in chunks, referred to by an iterator (not an index)
            )
        {
            const size_t sweep = globalts / _totalframes;    // which sweep (this determines randomization)
            if (sweep == currentsweep)                       // already got this one--nothing to do
                return sweep;

            currentsweep = sweep;
            if (verbosity > 0)
                fprintf (stderr, "lazyrandomization: re-randomizing for sweep %d in %s mode\n", (int)currentsweep, framemode ? "frame" : "utterance");

            const size_t sweepts = sweep * _totalframes;     // first global frame index for this sweep

            // first randomize chunks
            std::vector<std::vector<std::vector<utterancechunkdata>::const_iterator>> randomizedchunkrefs;
            foreach_index (i, allchunks)
                randomizedchunkrefs.push_back(std::vector<std::vector<utterancechunkdata>::const_iterator>());

            foreach_index (i, allchunks)
                randomizedchunkrefs[i].reserve (allchunks[i].size());

            foreach_index (i, allchunks)    // TODO: this cries for iterating using the iterator!
            {
                foreach_index(j, allchunks[i])
                    randomizedchunkrefs[i].push_back (allchunks[i].begin() + j);
                assert (randomizedchunkrefs[i].size() == allchunks[i].size());

                // note that since randomshuffle() uses sweep as seed, this will keep the randomization common across all feature streams
                randomshuffle (randomizedchunkrefs[i], sweep); // bring into random order (with random seed depending on sweep)
            }

            // place them onto the global timeline -> randomizedchunks[]
            // We are processing with randomization within a rolling window over this chunk sequence.
            // Paging will happen on a chunk-by-chunk basis.
            // The global time stamp is needed to determine the paging window.
            randomizedchunks.clear();               // data chunks after being brought into random order (we randomize within a rolling window over them)

            foreach_index(i, allchunks)
                randomizedchunks.push_back(std::vector<chunk>());

            foreach_index(i, allchunks)
            {
                randomizedchunks[i].reserve (randomizedchunkrefs[i].size());
                foreach_index (k, randomizedchunkrefs[i])
                    randomizedchunks[i].push_back (chunk (randomizedchunkrefs[i][k], randomizedchunks[i].empty() ? 0 : randomizedchunks[i].back().utteranceposend(), randomizedchunks[i].empty() ? sweepts : randomizedchunks[i].back().globalte()));
                assert (randomizedchunks[i].size() == allchunks[i].size());
                assert (randomizedchunks[i].empty() || (randomizedchunks[i].back().utteranceposend() == numutterances && randomizedchunks[i].back().globalte() == sweepts + _totalframes));
            }

            // for each chunk, compute the randomization range (w.r.t. the randomized chunk sequence)
            foreach_index (i, randomizedchunks)
            {
                // Only required for first feature stream
                if (i != 0)
                    continue;

                foreach_index (k, randomizedchunks[i])
                {
                    chunk & chunk = randomizedchunks[i][k];
                    // start with the range of left neighbor
                    if (k == 0)
                    {
                        chunk.windowbegin = 0;
                        chunk.windowend = 1;
                    }
                    else
                    {
                        chunk.windowbegin = randomizedchunks[i][k-1].windowbegin;  // might be too early
                        chunk.windowend = randomizedchunks[i][k-1].windowend;      // might have more space
                    }
                    while (chunk.globalts - randomizedchunks[i][chunk.windowbegin].globalts > randomizationrange/2)
                        chunk.windowbegin++;            // too early
                    while (chunk.windowend < randomizedchunks[i].size() && randomizedchunks[i][chunk.windowend].globalte() - chunk.globalts < randomizationrange/2)
                        chunk.windowend++;              // got more space
                }
            }

            // This completes chunk randomization.
            // Now set up the following members for sequence randomization (i.e., utterance or frame):
            //  - positionchunkwindows
            //  - randomizedutterancerefs - this is the data structure being shuffled
            //  - randomizedutteranceposmap

            // TODO adapt comments below. TODO test in utterance mode
            // We will now introduce the concept of utterance *position*.
            // During processing, utterances will be indexed by position (which is in turn derived from a frame index in getbatch()),
            // and it is assumed (required) that positions are requested consecutively.
            // Each utterance position has an underlying associated utterance, which is represented as (chunkid, within-chunk index) and randomly assigned.
            // Each utterance position also has an associated range of chunks that are kept in memory,
            // and the associated underlying utterance is guaranteed to be found within that associated range of chunks.
            // That allows to page out/in data when processing utterance positions in a consecutive manner.

            // compute chunk windows for every utterance position -> positionchunkwindows[]
            // Utterance positions can only reference underlying utterance data within the chunk window.
            // Utterance positions are defined by the randomized chunk sequence (i.e. their underlying 'defining' chunk differs from sweep to sweep).
            size_t numsequences = framemode ? _totalframes : numutterances;

            positionchunkwindows.clear();           // [utterance position] -> [windowbegin, windowend) for controlling paging
            positionchunkwindows.reserve(numsequences);

            // positionchunkwindows should be consistent for all inputs (distinct feature streams), so just build based on feature[0]
            // contains pointer to chunk elements but only to compute index
            foreach_index (k, randomizedchunks[0]) // TODO: this really cries for iterating using iterators!
            {
                chunk & chunk = randomizedchunks[0][k];
                for (size_t i = 0; i < chunk.numutterances(); i++)  // loop over utterances in this chunk
                {
                    size_t numsequences = framemode ? chunk.getchunkdata().numframes(i) : 1;
                    for (size_t m = 0; m < numsequences; m++)
                    {
                        positionchunkwindows.push_back(randomizedchunks[0].begin() + k);
                    }
                }
            }
            assert(positionchunkwindows.size() == (framemode ? _totalframes : numutterances));

            // build the randomized utterances array -> randomizedutterancerefs[]
            // start by assigning all utterance positions to utterances in non-random consecutive manner
            randomizedutterancerefs.clear();        // [pos] randomized utterance ids
            randomizedutterancerefs.reserve(numsequences);
            foreach_index (k, randomizedchunks[0])
            {
                chunk & chunk = randomizedchunks[0][k];
                for (size_t i = 0; i < chunk.numutterances(); i++)  // loop over utterances in this chunk
                {
                    size_t numsequences = framemode ? chunk.getchunkdata().numframes(i) : 1;
                    for (size_t m = 0; m < numsequences; m++)
                    {
                        randomizedutterancerefs.push_back (sequenceref /* utteranceref */ (k, i, m));
                    }
                }
            }
            assert(randomizedutterancerefs.size() == numsequences);

            // check we got those setup right
            foreach_index (i, randomizedutterancerefs)
            {
                auto & uttref = randomizedutterancerefs[i];
                assert(positionchunkwindows[i].isvalidforthisposition(uttref)); uttref;
            }

            // we now randomly shuffle randomizedutterancerefs[pos], while considering the constraints of what chunk range needs to be in memory
            srand ((unsigned int) sweep + 1);
            for (size_t i = 0; i < randomizedutterancerefs.size(); i++)
            {
                // get valid randomization range, expressed in chunks
                const size_t windowbegin = positionchunkwindows[i].windowbegin();
                const size_t windowend =   positionchunkwindows[i].windowend();

                // get valid randomization range, expressed in utterance positions
                // Remember, utterance positions are defined by chunks.
                size_t posbegin;
                size_t posend;

                // TODO abstract across these (should be sequence indices...)
                if (framemode)
                {
                    // in frames
                    posbegin = randomizedchunks[0][windowbegin].globalts   - sweepts;
                    posend =   randomizedchunks[0][windowend-1].globalte() - sweepts;
                }
                else
                {
                    posbegin = randomizedchunks[0][windowbegin].utteranceposbegin;
                    posend =   randomizedchunks[0][windowend-1].utteranceposend();
                }

                // randomization range for this utterance position is [posbegin, posend)
                for(;;)
                {
                    // pick a random location
                    const size_t j = msra::dbn::rand (posbegin, posend);    // a random number within the window
                    if (i == j)
                        break;  // the random gods say "this one points to its original position"... nothing wrong about that, but better not try to swap

                    // We want to swap utterances at i and j, but need to make sure they remain in their allowed range.
                    // This is guaranteed for a so-far untouched utterance, but both i and j may have been touched by a previous swap.

                    // We want to use the utterance previously referenced at utterance position j at position i. Is that allowed?
                    if (!positionchunkwindows[i].isvalidforthisposition (randomizedutterancerefs[j]))
                        continue;   // nope --try another

                    // Likewise may we use the utterance previously referenced at utterance position i at position j?
                    if (!positionchunkwindows[j].isvalidforthisposition (randomizedutterancerefs[i]))
                        continue;   // nope --try another

                    // yep--swap them
                    ::swap (randomizedutterancerefs[i], randomizedutterancerefs[j]); // TODO old swap was perhaps more efficient
                    break;
                }
            }

            size_t t = sweepts;
            foreach_index (i, randomizedutterancerefs)
            {
                auto & uttref = randomizedutterancerefs[i];
                uttref.globalts = t;
                if (framemode)
                {
                    uttref.numframes = 1;
                }
                else
                {
                    uttref.numframes = randomizedchunks[0][uttref.chunkindex].getchunkdata().numframes (uttref.utteranceindex);
                }


                t = uttref.globalte();
            }
            assert (t == sweepts + _totalframes); // TODO does this hold if there we invalid utterance at the end of a chunk?

            // verify that we got it right (I got a knot in my head!)
            foreach_index (i, randomizedutterancerefs)
            {
                // get utterance referenced at this position
                const auto & uttref = randomizedutterancerefs[i];
                // check if it is valid for this position
                if (uttref.chunkindex < positionchunkwindows[i].windowbegin() || uttref.chunkindex >= positionchunkwindows[i].windowend())
                    LogicError("lazyrandomization: randomization logic mangled!");
            }

            // create lookup table for (globalts values -> pos) -> randomizedutteranceposmap[]
            randomizedutteranceposmap.clear();      // [globalts] -> pos lookup table
            foreach_index (pos, randomizedutterancerefs)
            {
                auto & uttref = randomizedutterancerefs[pos];
                randomizedutteranceposmap[uttref.globalts] = (size_t) pos;
            }

            // TODO refactor into method
            // check it --my head spins
            t = 0;
            foreach_index (i, randomizedchunks[0])
            {
                const auto & chunk = randomizedchunks[0][i];       // for window and chunkdata
                const size_t poswindowbegin = chunk.windowbegin;
                const size_t poswindowend = chunk.windowend;

                const auto & chunkdata = chunk.getchunkdata();  // for numutterances/numframes
                const size_t numutt = chunkdata.numutterances();
                for (size_t k = 0; k < numutt; k++)
                {
                    const size_t n = framemode ? chunkdata.numframes (k) : 1;
                    for (size_t m = 0; m < n; m++)
                    {
                        //const size_t randomizedchunkindex = randomizedframerefs[t].chunkindex;
                        const size_t randomizedchunkindex = randomizedutterancerefs[t].chunkindex;
                        if (randomizedchunkindex < poswindowbegin || randomizedchunkindex >= poswindowend)
                            LogicError("lazyrandomization: nope, you got frame randomization wrong, dude");
                        t++;
                    }
                }
            }
            assert (t == numsequences);

            return sweep;
        }

        size_t chunkforframepos(const size_t t) const  // find chunk for a given frame position
        {
            //inspect chunk of first feature stream only
            auto iter = std::lower_bound(randomizedchunks[0].begin(), randomizedchunks[0].end(), t, [&](const chunk & chunk, size_t t) { return chunk.globalte() <= t; });
            const size_t chunkindex = iter - randomizedchunks[0].begin();
            if (t < randomizedchunks[0][chunkindex].globalts || t >= randomizedchunks[0][chunkindex].globalte())
                LogicError("chunkforframepos: dude, learn STL!");
            return chunkindex;
        }

        randomizer(int verbosity, bool framemode, size_t totalframes, size_t numutterances, size_t randomizationrange)
            : verbosity(verbosity)
            , framemode(framemode)
            , _totalframes(totalframes)
            , numutterances(numutterances)
            , randomizationrange(randomizationrange)
            , currentsweep(SIZE_MAX)
        {
        }

        const utterancechunkdata & getChunkData(size_t streamIndex, size_t randomizedChunkIndex)
        {
            assert(streamIndex < randomizedchunks.size());
            assert(randomizedChunkIndex < randomizedchunks[streamIndex].size());
            return randomizedchunks[streamIndex][randomizedChunkIndex].getchunkdata();
        }

        size_t getChunkWindowBegin(size_t randomizedChunkIndex)
        {
            const size_t streamIndex = 0;
            assert(randomizedChunkIndex < randomizedchunks[streamIndex].size());
            return randomizedchunks[streamIndex][randomizedChunkIndex].windowbegin;
        }

        size_t getChunkWindowEnd(size_t randomizedChunkIndex)
        {
            const size_t streamIndex = 0;
            assert(randomizedChunkIndex < randomizedchunks[streamIndex].size());
            return randomizedchunks[streamIndex][randomizedChunkIndex].windowend;
        }

        size_t getNumSequences()
        {
            return randomizedutterancerefs.size();
        }

        const sequenceref & getSequenceRef(size_t sequenceIndex)
        {
            assert(sequenceIndex < randomizedutterancerefs.size());
            return randomizedutterancerefs[sequenceIndex];
        }
    };

    std::unique_ptr<randomizer> rand;

    // helper to page out a chunk with log message
    void releaserandomizedchunk(size_t k)
    {
        size_t numreleased = 0;
        size_t numStreams = allchunks.size();
        for (size_t m = 0; m < numStreams; m++)
        {
            auto & chunkdata = rand->getChunkData(m, k);
            if (chunkdata.isinram())
            {
#if 0 // TODO restore diagnostics
                if (verbosity)
                    fprintf(stderr, "releaserandomizedchunk: paging out randomized chunk %u (frame range [%d..%d]), %d resident in RAM\n",
                    (int)k, (int)randomizedchunks[m][k].globalts, (int)(randomizedchunks[m][k].globalte() - 1), (int)(chunksinram - 1));
#endif
                chunkdata.releasedata();
                numreleased++;
            }
        }
        if (numreleased>0 && numreleased<numStreams)
        {
            LogicError("releaserandomizedchunk: inconsistency detected - some inputs have chunks in ram, some not");
        }
        else if (numreleased == numStreams)
        {
            chunksinram--;
        }
        return;
    }

    // helper to page in a chunk for a given utterance
    // (window range passed in for checking only)
    // Returns true if we actually did read something.
    bool requirerandomizedchunk(const size_t chunkindex, const size_t windowbegin, const size_t windowend)
    {
        size_t numinram = 0;

        if (chunkindex < windowbegin || chunkindex >= windowend)
            LogicError("requirerandomizedchunk: requested utterance outside in-memory chunk range");

        size_t numStreams = allchunks.size();
        for (size_t m = 0; m < numStreams; m++)
        {
            auto & chunkdata = rand->getChunkData(m, chunkindex);
            if (chunkdata.isinram())
                numinram++;
        }
        if (numinram == numStreams)
            return false;
        else if (numinram == 0)
        {
            for (size_t m = 0; m < numStreams; m++)
            {
                auto & chunkdata = rand->getChunkData(m, chunkindex);
#if 0 // TODO restore diagnostics
                if (verbosity)
                    fprintf(stderr, "feature set %u: requirerandomizedchunk: paging in randomized chunk %llu (frame range [%llu..%llu]), %llu resident in RAM\n",
                    m, chunkindex, chunk.globalts, (chunk.globalte() - 1), (chunksinram + 1));
#endif
                msra::util::attempt(5, [&]()   // (reading from network)
                {
                    chunkdata.requiredata(featkind[m], featdim[m], sampperiod[m], lattices, verbosity);
                });
            }
            chunksinram++;
            return true;
        }
        else
        {
            LogicError("requirerandomizedchunk: inconsistency detected - some inputs need chunks paged in, some not");
        }
    }

    // TODO: this may go away if we store classids directly in the utterance data
    template<class VECTOR> class shiftedvector  // accessing a vector with a non-0 starting index
    {
        void operator= (const shiftedvector &);
        VECTOR & v;
        size_t first;
        size_t n;
        void check (size_t i) const { if (i >= n) LogicError("shiftedvector: index out of bounds"); }
    public:
        shiftedvector (VECTOR & v, size_t first, size_t n) : v (v), first (first), n (n) { }
        // TODO: the following is not templated--do it if needed; also should return a const reference then
        size_t operator[] (size_t i) const { check (i); return v[first + i]; }
    };
    template<class UTTREF> std::vector<shiftedvector<biggrowablevector<CLASSIDTYPE>>> getclassids (const UTTREF & uttref)  // return sub-vector of classids[] for a given utterance
    {
        std::vector<shiftedvector<biggrowablevector<CLASSIDTYPE>>> allclassids;
        allclassids.empty();

        if (!issupervised())
        {
            foreach_index(i,classids)
                allclassids.push_back(std::move(shiftedvector<biggrowablevector<CLASSIDTYPE>> ((*classids[i]), 0, 0)));
            return allclassids;     // nothing to return
        }
        const auto & chunkdata = rand->getChunkData(0, uttref.chunkindex);
        const size_t classidsbegin = chunkdata.getclassidsbegin (uttref.utteranceindex); // index of first state label in global concatenated classids[] array
        const size_t n = chunkdata.numframes (uttref.utteranceindex);
        foreach_index(i,classids)
        {
            if ((*classids[i])[classidsbegin + n] != (CLASSIDTYPE) -1)
                LogicError("getclassids: expected boundary marker not found, internal data structure screwed up");
            allclassids.push_back(std::move(shiftedvector<biggrowablevector<CLASSIDTYPE>> ((*classids[i]), classidsbegin, n)));
        }
        return allclassids;   // nothing to return
    }
    template<class UTTREF> std::vector<shiftedvector<biggrowablevector<HMMIDTYPE>>> getphonebound(const UTTREF & uttref)  // return sub-vector of classids[] for a given utterance
    {
        std::vector<shiftedvector<biggrowablevector<HMMIDTYPE>>> allphoneboundaries;
        allphoneboundaries.empty();

        if (!issupervised())
        {
            foreach_index(i, classids)
                allphoneboundaries.push_back(std::move(shiftedvector<biggrowablevector<HMMIDTYPE>>((*phoneboundaries[i]), 0, 0)));
            return allphoneboundaries;     // nothing to return
        }
        const auto & chunk = randomizedchunks[0][uttref.chunkindex];
        const auto & chunkdata = chunk.getchunkdata();
        const size_t classidsbegin = chunkdata.getclassidsbegin(uttref.utteranceindex); // index of first state label in global concatenated classids[] array
        const size_t n = chunkdata.numframes(uttref.utteranceindex);
        foreach_index(i, phoneboundaries)
        {
            if ((*phoneboundaries[i])[classidsbegin + n] != (HMMIDTYPE)-1)
                LogicError("getclassids: expected boundary marker not found, internal data structure screwed up");
            allphoneboundaries.push_back(std::move(shiftedvector<biggrowablevector<HMMIDTYPE>>((*phoneboundaries[i]), classidsbegin, n)));
        }
        return allphoneboundaries;   // nothing to return
    }

public:
    // constructor
    // Pass empty labels to denote unsupervised training (so getbatch() will not return uids).
    // This mode requires utterances with time stamps.
    minibatchutterancesourcemulti (const std::vector<std::vector<wstring>> & infiles, const std::vector<map<wstring,std::vector<msra::asr::htkmlfentry>>> & labels,
                              std::vector<size_t> vdim, std::vector<size_t> udim, std::vector<size_t> leftcontext, std::vector<size_t> rightcontext, size_t randomizationrange,
                              const latticesource & lattices, const map<wstring,msra::lattices::lattice::htkmlfwordsequence> & allwordtranscripts, const bool framemode)
        : vdim (vdim), leftcontext(leftcontext), rightcontext(rightcontext), sampperiod (0), featdim (0),
          lattices (lattices), allwordtranscripts (allwordtranscripts), framemode (framemode), chunksinram (0), timegetbatch (0), verbosity(2)
        // [v-hansu] change framemode (lattices.empty()) into framemode (false) to run utterance mode without lattice
        // you also need to change another line, search : [v-hansu] comment out to run utterance mode without lattice
    {
        // process infiles to know dimensions of things (but not loading features)
        size_t nomlf = 0;                       // number of entries missing in MLF (diagnostics)
        size_t nolat = 0;                       // number of entries missing in lattice archive (diagnostics)
        std::vector<size_t> numclasses;                  // number of output classes as found in the label file (diagnostics)
        _totalframes = 0;
        wstring key;
        size_t numutts=0;

        std::vector<bool> uttisvalid; // boolean flag to check that utterance is valid. valid means number of
                                      //frames is consistent across all feature and label streams
        std::vector<size_t> uttduration; // track utterance durations to determine utterance validity

        std::vector<size_t> classidsbegin;

        assert(infiles.size() == 1); // we are only looking at a single file here...
        assert(leftcontext.size() == 1);
        assert(leftcontext[0] == 0);
        assert(rightcontext.size() == 1);
        assert(rightcontext[0] == 0);
        assert(labels.size() == 1); // only have one

        allchunks = std::vector<std::vector<utterancechunkdata>>(infiles.size(), std::vector<utterancechunkdata>());
        featdim = std::vector<size_t>(infiles.size(), 0);
        sampperiod = std::vector<unsigned int>(infiles.size(), 0);
        featkind = std::vector<string>(infiles.size(), "");

        numclasses = std::vector<size_t>(labels.size(), 0);
        counts = std::vector<std::vector<size_t>>(labels.size(), std::vector<size_t>());

        foreach_index (i, labels)
        {
            classids.push_back(unique_ptr<biggrowablevector<CLASSIDTYPE>>(new biggrowablevector<CLASSIDTYPE>()));
            phoneboundaries.push_back(unique_ptr<biggrowablevector<HMMIDTYPE>>(new biggrowablevector<HMMIDTYPE>()));
            //std::pair<std::vector<wstring>,std::vector<wstring>> latticetocs;
            //std::unordered_map<std::string,size_t> modelsymmap;
            //lattices.push_back(shared_ptr<latticesource>(new latticesource(latticetocs, modelsymmap)));
        }


        // first check consistency across feature streams
        // We'll go through the SCP files for each stream to make sure the duration is consistent
        // If not, we'll plan to ignore the utterance, and inform the user
        // m indexes the feature stream
        // i indexes the files within a stream, i.e. in the SCP file
        foreach_index(m, infiles) {
            if (m == 0) {
                numutts = infiles[m].size();
                uttisvalid = std::vector<bool>(numutts, true);
                uttduration = std::vector<size_t>(numutts, 0);
            }
            else if (infiles[m].size()!=numutts)
                RuntimeError("minibatchutterancesourcemulti: all feature files must have same number of utterances");

            foreach_index(i, infiles[m]){
                utterancedesc utterance(msra::asr::htkfeatreader::parsedpath(infiles[m][i]), 0);  //mseltzer - is this foolproof for multiio? is classids always non-empty?
                const size_t uttframes = utterance.numframes(); // will throw if frame bounds not given --required to be given in this mode
                // we need at least 2 frames for boundary markers to work
                if (uttframes < 2)
                    RuntimeError("minibatchutterancesource: utterances < 2 frames not supported");
                if (uttframes > 65535 /* TODO frameref::maxframesperutterance */)
                {
                    fprintf(stderr, "minibatchutterancesource: skipping %d-th file (%d frames) because it exceeds max. frames (%d) for frameref bit field: %ls\n", i, (int)uttframes, (int)65535 /* frameref::maxframesperutterance */, key.c_str());
                    uttduration[i] = 0;
                    uttisvalid[i] = false;
                }
                else
                {
                    if (m == 0){
                        uttduration[i] = uttframes;
                        uttisvalid[i] = true;
                    }
                    else if (uttduration[i] != uttframes){
                                fprintf(stderr, "minibatchutterancesource: skipping %d-th file due to inconsistency in duration in different feature streams (%d vs %d frames)\n", i, (int)uttduration[i], (int)uttframes);
                        uttduration[i] = 0;
                        uttisvalid[i] = false;
                    }
                }
            }
        }

        // shouldn't this be checked (again) later? more utterances can be invalidated...
        size_t invalidutts=0;
        foreach_index(i, uttisvalid) {
            if (!uttisvalid[i])
                invalidutts++;
        }
        assert(invalidutts == 0); // For us it's zero
        if (invalidutts > uttisvalid.size() / 2)
                    RuntimeError("minibatchutterancesource: too many files with inconsistent durations, assuming broken configuration\n");
        else if (invalidutts>0)
                    fprintf(stderr, "Found inconsistent durations across feature streams in %d out of %d files\n", (int)invalidutts, (int)uttisvalid.size());


        // now process the features and labels
        size_t utterancesetsize = 0;
        foreach_index (m, infiles)
        {
            std::vector<utterancedesc> utteranceset;// read all utterances to here first; at the end, distribute to chunks
            utteranceset.reserve(infiles[m].size());
                    //if (m==0)
                    //    numutts = infiles[m].size();
                    //else
                    //    if (infiles[m].size()!=numutts)
                    //        RuntimeError("minibatchutterancesourcemulti: all feature files must have same number of utterances\n");
            if (m==0)
                classidsbegin.clear();

            foreach_index (i, infiles[m])
            {
                if (i % (infiles[m].size() / 100 + 1) == 0) { fprintf (stderr, "."); fflush (stderr); }
                // build utterance descriptor
                if (m == 0 && !labels.empty())
                    classidsbegin.push_back(classids[0]->size());

                if (uttisvalid[i])
                {
                    utterancedesc utterance (msra::asr::htkfeatreader::parsedpath (infiles[m][i]), labels.empty() ? 0 : classidsbegin[i] );  //mseltzer - is this foolproof for multiio? is classids always non-empty?
                    const size_t uttframes = utterance.numframes(); // will throw if frame bounds not given --required to be given in this mode
                        assert(uttframes == uttduration[i]); // ensure nothing funky happened
                        // already performed these checks above
                        // we need at least 2 frames for boundary markers to work
                        //if (uttframes < 2)
                        //    RuntimeError("minibatchutterancesource: utterances < 2 frames not supported");
                        //if (uttframes > frameref::maxframesperutterance)
                        //{
                        //    fprintf (stderr, "minibatchutterancesource: skipping %d-th file (%d frames) because it exceeds max. frames (%d) for frameref bit field: %ls", i, uttframes, frameref::maxframesperutterance, key.c_str());
                        //    continue;
                        //}

                    // check whether we have the ref transcript
                    bool lacksmlf = true;
                    if (!labels.empty())    // empty means unsupervised mode (don't load any)
                    {
                        key = utterance.key();
                        // check if labels are available (if not, it normally means that no path was found in realignment)
                        auto labelsiter = labels[0].find (key);
                        //const bool lacksmlf = (labelsiter == labels[0].end());
                        lacksmlf = (labelsiter == labels[0].end());
                        if (lacksmlf)
                            if (nomlf++ < 5)
                                fprintf (stderr, " [no labels for  %ls]", key.c_str());
                        // check if lattice is available (when in lattice mode)
                        // TODO: also check the #frames here; requires a design change of the TOC format & a rerun
                        const bool lackslat = !lattices.empty() && !lattices.haslattice (key); // ('true' if we have no lattices)
                        if (lackslat)
                            if (nolat++ < 5)
                                fprintf (stderr, " [no lattice for %ls]", key.c_str());
                        // skip if either one is missing
                            if (lacksmlf || lackslat){
                                uttisvalid[i] = false;
                            continue;   // skip this utterance at all
                    }
                        }
                    // push the label sequence into classids[], since we already looked it up
                    // TODO: we can store labels more efficiently now since we don't do frame-wise random access anymore.

                    // OK, utterance has all we need --remember it

                    if (m==0)
                    {
                        if (!labels.empty() && !lacksmlf)
                        {
                            // first verify that all the label files have the proper duration
                            foreach_index (j, labels)
                            {
                                const auto & labseq = labels[j].find(key)->second;
                                // check if durations match; skip if not
                                size_t labframes = labseq.empty() ? 0 : (labseq[labseq.size()-1].firstframe + labseq[labseq.size()-1].numframes);
                                if (labframes != uttframes)
                                {
                                    fprintf (stderr, " [duration mismatch (%d in label vs. %d in feat file), skipping %ls]", (int)labframes, (int)uttframes, key.c_str());
                                    nomlf++;
                                    uttisvalid[i] = false;
                                    //continue;   // skip this utterance at all
                                    break;
                                }
                            }
                                if (uttisvalid[i])
                                {
                                utteranceset.push_back(std::move(utterance));
                                _totalframes += uttframes;
                                // then parse each mlf if the durations are consistent
                                foreach_index(j, labels)
                                {
                                    const auto & labseq = labels[j].find(key)->second;
                                    // expand classid sequence into flat array
                                    foreach_index (i, labseq)
                                    {
                                        const auto & e = labseq[i];
                                        if ((i > 0 && labseq[i - 1].firstframe + labseq[i - 1].numframes != e.firstframe) || (i == 0 && e.firstframe != 0))
                                        {
                                            RuntimeError("minibatchutterancesource: labels not in consecutive order MLF in label set: %ls", key.c_str());
                                            // TODO Why will these yield a run-time error as opposed to making the utterance invalid?
                                            // TODO check this at the source. Saves storing numframes field.
                                        }
                                        if (e.classid >= udim[j])
                                        {
                                            RuntimeError("minibatchutterancesource: class id %d exceeds model output dimension %d in file %ls", (int)e.classid, (int)udim[j], key.c_str());
                                        }
                                        if (e.classid != (CLASSIDTYPE) e.classid)
                                            RuntimeError("CLASSIDTYPE has too few bits");
                                        for (size_t t = e.firstframe; t < e.firstframe + e.numframes; t++)
                                        {
                                            classids[j]->push_back (e.classid);
                                            if (e.phonestart != 0 && t == e.firstframe)
                                                phoneboundaries[j]->push_back((HMMIDTYPE)e.phonestart);
                                            else
                                                phoneboundaries[j]->push_back((HMMIDTYPE)0);
                                        }
                                        numclasses[j] = max (numclasses[j], (size_t)(1u + e.classid));
                                        counts[j].resize (numclasses[j], 0);
                                        counts[j][e.classid] += e.numframes;
                                    }

                                    classids[j]->push_back ((CLASSIDTYPE) -1);  // append a boundary marker marker for checking
                                    phoneboundaries[j]->push_back((HMMIDTYPE)-1); // append a boundary marker marker for checking

                                    if (!labels[j].empty() && classids[j]->size() != _totalframes + utteranceset.size())
                                        LogicError("minibatchutterancesource: label duration inconsistent with feature file in MLF label set: %ls", key.c_str());
                                    assert (labels[j].empty() || classids[j]->size() == _totalframes + utteranceset.size());
                                }
                            }
                        }
                        else
                        {
                                assert(classids.empty() && labels.empty());
                                utteranceset.push_back(std::move(utterance));
                                _totalframes += uttframes;
                        }
                    }
                    else
                    {
                        utteranceset.push_back(std::move(utterance));
                    }
                }
            }
            if (m == 0)
                utterancesetsize = utteranceset.size();
            else
                assert(utteranceset.size() == utterancesetsize);

            fprintf (stderr, "feature set %d: %d frames in %d out of %d utterances\n", m, (int)_totalframes, (int)utteranceset.size(), (int)infiles[m].size());

            if (!labels.empty()){
                foreach_index (j, labels){
                    biggrowablevector<CLASSIDTYPE> & cid = *classids[j];
                    foreach_index (i, utteranceset){
                        //if ((*classids[j])[utteranceset[i].classidsbegin + utteranceset[i].numframes()] != (CLASSIDTYPE) -1)
                        //printf("index = %d\n",utteranceset[i].classidsbegin + utteranceset[i].numframes());
                        //printf("cid[index] = %d\n",cid[utteranceset[i].classidsbegin + utteranceset[i].numframes()]);
                        //printf("CLASSIDTYPE(-1) = %d\n",(CLASSIDTYPE) -1);
                        if (cid[utteranceset[i].classidsbegin + utteranceset[i].numframes()] != (CLASSIDTYPE) -1)
                            LogicError("minibatchutterancesource: classids[] out of sync");
                    }
                }
            }
            if (nomlf + nolat > 0)
            {
                fprintf (stderr, "minibatchutterancesource: out of %d files, %d files not found in label set and %d have no lattice\n", (int)infiles[0].size(), (int)nomlf, (int)nolat);
                if (nomlf + nolat > infiles[m].size() / 2)
                    RuntimeError("minibatchutterancesource: too many files not found in label set--assuming broken configuration\n");
            }
            assert(nomlf + nolat == 0); // For us it's zero
            if (m==0) {foreach_index(j, numclasses) { fprintf(stderr,"label set %d: %d classes\n", j, (int)numclasses[j]); } }
            // distribute them over chunks
            // We simply count off frames until we reach the chunk size.
            // Note that we first randomize the chunks, i.e. when used, chunks are non-consecutive and thus cause the disk head to seek for each chunk.
            const size_t framespersec = 100;                    // we just assume this; our efficiency calculation is based on this
            const size_t chunkframes = 15 * 60 * framespersec;  // number of frames to target for each chunk
            // Loading an initial 24-hour range will involve 96 disk seeks, acceptable.
            // When paging chunk by chunk, chunk size ~14 MB.
            std::vector<utterancechunkdata> & thisallchunks = allchunks[m];

            thisallchunks.resize (0);
            thisallchunks.reserve (_totalframes / chunkframes); // This is ignoring I/O for invalid utterances... // TODO round up?

            foreach_index (i, utteranceset)
            {
                // if exceeding current entry--create a new one
                // I.e. our chunks are a little larger than wanted (on av. half the av. utterance length).
                if (thisallchunks.empty() || thisallchunks.back().totalframes > chunkframes || thisallchunks.back().numutterances() >= 65535 /* TODO frameref::maxutterancesperchunk */ )
                // TODO > instead of >= ? if (thisallchunks.empty() || thisallchunks.back().totalframes > chunkframes || thisallchunks.back().numutterances() >= frameref::maxutterancesperchunk)
                    thisallchunks.push_back (utterancechunkdata());
                // append utterance to last chunk
                utterancechunkdata & currentchunk = thisallchunks.back();
                currentchunk.push_back (std::move (utteranceset[i]));    // move it out from our temp array into the chunk
                // TODO: above push_back does not actually 'move' because the internal push_back does not accept that
            }
            numutterances = utteranceset.size();
            fprintf (stderr, "minibatchutterancesource: %d utterances grouped into %d chunks, av. chunk size: %.1f utterances, %.1f frames\n",
                (int)numutterances, (int)thisallchunks.size(), numutterances / (double) thisallchunks.size(), _totalframes / (double) thisallchunks.size());
            // Now utterances are stored exclusively in allchunks[]. They are never referred to by a sequential utterance id at this point, only by chunk/within-chunk index.

            // Initialize the randomizer
            rand = std::make_unique<randomizer>(verbosity, framemode, _totalframes, numutterances, randomizationrange);
        }
    }

private:
    class matrixasvectorofvectors  // wrapper around a matrix that views it as a vector of column vectors
    {
        void operator= (const matrixasvectorofvectors &);  // non-assignable
        msra::dbn::matrixbase & m;
    public:
        matrixasvectorofvectors (msra::dbn::matrixbase & m) : m (m) {}
        size_t size() const { return m.cols(); }
        const_array_ref<float> operator[] (size_t j) const { return array_ref<float> (&m(0,j), m.rows()); }
    };


public:

    void setverbosity(int newverbosity){ verbosity = newverbosity; }

    // get the next minibatch
    // A minibatch is made up of one or more utterances.
    // We will return less than 'framesrequested' unless the first utterance is too long.
    // Note that this may return frames that are beyond the epoch end, but the first frame is always within the epoch.
    // We specify the utterance by its global start time (in a space of a infinitely repeated training set).
    // This is efficient since getbatch() is called with sequential 'globalts' except at epoch start.
    // Note that the start of an epoch does not necessarily fall onto an utterance boundary. The caller must use firstvalidglobalts() to find the first valid globalts at or after a given time.
    // Support for data parallelism:  If mpinodes > 1 then we will
    //  - load only a subset of blocks from the disk
    //  - skip frames/utterances in not-loaded blocks in the returned data
    //  - 'framesadvanced' will still return the logical #frames; that is, by how much the global time index is advanced
    bool getbatch(const size_t globalts, const size_t framesrequested,
                  const size_t subsetnum, const size_t numsubsets, size_t & framesadvanced,
                  std::vector<msra::dbn::matrix> & feat, std::vector<std::vector<size_t>> & uids,
                  std::vector<const_array_ref<msra::lattices::lattice::htkmlfwordsequence::word>> & transcripts,
                  std::vector<shared_ptr<const latticesource::latticepair>> & latticepairs, std::vector<std::vector<size_t>> & sentendmark,
                  std::vector<std::vector<size_t>> & phoneboundaries) override
    {
        bool readfromdisk = false;  // return value: shall be 'true' if we paged in anything

        auto_timer timergetbatch;
        assert (_totalframes > 0);

        // update randomization if a new sweep is entered  --this is a complex operation that updates many of the data members used below
        const size_t sweep = rand->lazyrandomization(globalts, allchunks);

        size_t mbframes = 0;
        const std::vector<char> noboundaryflags;    // dummy

        sentendmark;
        phoneboundaries;
#undef EXPERIMENTAL_UNIFIED_PATH
#ifdef EXPERIMENTAL_UNIFIED_PATH
        // find utterance position for globalts
        // There must be a precise match; it is not possible to specify frames that are not on boundaries.
        auto positer = randomizedutteranceposmap.find (globalts);
        if (positer == randomizedutteranceposmap.end())
            LogicError("getbatch: invalid 'globalts' parameter; must match an existing utterance boundary");
        const size_t spos = positer->second;

        size_t numsequences = framemode ? _totalframes : numutterances;

        // determine how many utterances will fit into the requested minibatch size
        mbframes = randomizedutterancerefs[spos].numframes;   // at least one utterance, even if too long
        size_t epos;
        for (epos = spos + 1; epos < numsequences /* numutterances */ && ((mbframes + randomizedutterancerefs[epos].numframes) < framesrequested); epos++)  // add more utterances as long as they fit within requested minibatch size
            mbframes += randomizedutterancerefs[epos].numframes;

        // do some paging housekeeping
        // This will also set the feature-kind information if it's the first time.
        // Free all chunks left of the range.
        // Page-in all chunks right of the range.
        // We are a little more blunt for now: Free all outside the range, and page in only what is touched. We could save some loop iterations.
        const size_t windowbegin = positionchunkwindows[spos].windowbegin();
        const size_t windowend =   positionchunkwindows[epos-1].windowend();
        for (size_t k = 0; k < windowbegin; k++)
            releaserandomizedchunk (k);
        for (size_t k = windowend; k < randomizedchunks[0].size(); k++)
            releaserandomizedchunk (k);
        for (size_t pos = spos; pos < epos; pos++)
            if ((randomizedutterancerefs[pos].chunkindex % numsubsets) == subsetnum)
                readfromdisk |= requirerandomizedchunk(randomizedutterancerefs[pos].chunkindex, windowbegin, windowend); // (window range passed in for checking only)

        // Note that the above loop loops over all chunks incl. those that we already should have.
        // This has an effect, e.g., if 'numsubsets' has changed (we will fill gaps).

        // determine the true #frames we return, for allocation--it is less than mbframes in the case of MPI/data-parallel sub-set mode
        size_t tspos = 0;
        for (size_t pos = spos; pos < epos; pos++)
        {
            const auto & uttref = randomizedutterancerefs[pos];
            if ((uttref.chunkindex % numsubsets) != subsetnum)            // chunk not to be returned for this MPI node
                continue;

            tspos += uttref.numframes;
        }

        // resize feat and uids
        feat.resize(vdim.size());
        uids.resize(classids.size());
        phoneboundaries.resize(classids.size());
        sentendmark.resize(vdim.size());
        assert(feat.size()==vdim.size());
        assert(feat.size()==randomizedchunks.size());

        // TODO should still work for !framemode; for framemode more work is needed:
        // - subsetsizes computation - augmentation still crashes
        foreach_index(i, feat)
        {
            feat[i].resize (vdim[i], tspos /* TODO versus allocframes */ );

            if (i==0)
            {
                foreach_index(j, uids)
                {
                    if (issupervised())             // empty means unsupervised training -> return empty uids
                    {
                        uids[j].resize(tspos);
                        phoneboundaries[j].resize(tspos);
                    }
                    else
                    {
                        uids[i].clear();
                        phoneboundaries[i].clear();
                    }
                    latticepairs.clear();               // will push_back() below
                    transcripts.clear();
                }
                foreach_index(j, sentendmark)
                {
                    sentendmark[j].clear();
                }
            }
        }

        if (verbosity > 0)
            fprintf(stderr, "getbatch: getting utterances %lu..%lu (%lu subset of %lu frames out of %lu requested) in sweep %lu\n",
                    spos, (epos - 1), tspos, mbframes, framesrequested, sweep);
        tspos = 0;   // relative start of utterance 'pos' within the returned minibatch
        for (size_t pos = spos; pos < epos; pos++)
        {
            const auto & uttref = randomizedutterancerefs[pos];
            if ((uttref.chunkindex % numsubsets) != subsetnum)            // chunk not to be returned for this MPI node
                continue;

            size_t n = 0;
            foreach_index(i, randomizedchunks)
            {
                const auto & chunk = randomizedchunks[i][uttref.chunkindex];
                const auto & chunkdata = chunk.getchunkdata();
                assert((numsubsets > 1) || (uttref.globalts == globalts + tspos));
                auto uttframes = chunkdata.getutteranceframes (uttref.utteranceindex);
                matrixasvectorofvectors uttframevectors (uttframes);    // (wrapper that allows m[j].size() and m[j][i] as required by augmentneighbors())
                n = uttref.numframes;
                const size_t uttNumFramesFromVector = uttframevectors.size();
                sentendmark[i].push_back(n + tspos);
                // TODO rejoin
                assert (uttNumFramesFromVector == uttframes.cols()); uttNumFramesFromVector;
                assert (n == (framemode ? 1 : uttNumFramesFromVector));
                assert (chunkdata.numframes (uttref.utteranceindex) == uttNumFramesFromVector);

                size_t frameIndex = uttref.frameindex;

                // copy the frames and class labels
                for (size_t t = 0; t < n; t++, frameIndex++)          // t = time index into source utterance
                {
                    size_t leftextent, rightextent;
                    // page in the needed range of frames
                    // TODO hoist?
                    if (leftcontext[i] == 0 && rightcontext[i] == 0)
                    {
                        leftextent = rightextent = augmentationextent(uttframevectors[frameIndex].size(), vdim[i]);
                    }
                    else
                    {
                        leftextent = leftcontext[i];
                        rightextent = rightcontext[i];
                    }

                    // TODO memory-safe, maybe not correct
                    augmentneighbors(uttframevectors, noboundaryflags, frameIndex, leftextent, rightextent, feat[i], t /* frameIndex */ + tspos);
                    //augmentneighbors(uttframevectors, noboundaryflags, frameIndex, leftextent, rightextent, feat[i], currmpinodeframecount);
                }

                // copy the frames and class labels
                if (i==0)
                {
                    auto uttclassids = getclassids (uttref);
                    auto uttphoneboundaries = getphonebound(uttref);
                    foreach_index(j, uttclassids)
                    {
                        for (size_t t = 0; t < n; t++)          // t = time index into source utterance
                        {
                            if (issupervised())
                            {
                                uids[j][t + tspos] = uttclassids[j][t];
                                phoneboundaries[j][t + tspos] = uttphoneboundaries[j][t];
                            }
                        }

                        if (!this->lattices.empty())
                        {
                            auto latticepair = chunkdata.getutterancelattice (uttref.utteranceindex);
                            latticepairs.push_back (latticepair);
                            // look up reference
                            const auto & key = latticepair->getkey();
                            if (!allwordtranscripts.empty())
                            {
                                const auto & transcript = allwordtranscripts.find (key)->second;
                                transcripts.push_back (transcript.words);
                            }
                        }
                    }
                }
            }
            tspos += n;
        }

        foreach_index(i, feat)
        {
            assert(tspos == feat[i].cols());
        }
#endif

        if (!framemode)      // regular utterance mode
        {
            assert(0); // looking at frame-mode scenario for now
            // TODO code was moved up
        }
        else
        {
            const size_t sweepts = sweep * _totalframes;         // first global frame index for this sweep
            const size_t sweepte = sweepts + _totalframes;       // and its end
            const size_t globalte = min (globalts + framesrequested, sweepte);  // we return as much as requested, but not exceeding sweep end
            mbframes = globalte - globalts;        // that's our mb size

            // determine window range
            // We enumerate all frames--can this be done more efficiently?
            const size_t firstchunk = rand->chunkforframepos (globalts);
            const size_t lastchunk = rand->chunkforframepos (globalte-1);

            assert(lastchunk <= firstchunk + 1); // shouldn't really cover more than two chunks...?
            const size_t windowbegin = rand->getChunkWindowBegin(firstchunk);
            const size_t windowend = rand->getChunkWindowEnd(lastchunk);
            const size_t numChunks = allchunks[0].size();
            const size_t numStreams = allchunks.size();
            if (verbosity > 0)
                fprintf (stderr, "getbatch: getting randomized frames [%d..%d] (%d frames out of %d requested) in sweep %d; chunks [%d..%d] -> chunk window [%d..%d)\n",
                     (int)globalts, (int)globalte, (int)mbframes, (int)framesrequested, (int)sweep, (int)firstchunk, (int)lastchunk, (int)windowbegin, (int)windowend);
            // release all data outside, and page in all data inside
            for (size_t k = 0; k < windowbegin; k++)
                releaserandomizedchunk (k);
            for (size_t k = windowbegin; k < windowend; k++)
                if ((k % numsubsets) == subsetnum)        // in MPI mode, we skip chunks this way
                    readfromdisk |= requirerandomizedchunk(k, windowbegin, windowend); // (window range passed in for checking only, redundant here)
            for (size_t k = windowend; k < numChunks; k++)
                releaserandomizedchunk (k);

            // determine the true #frames we return--it is less than mbframes in the case of MPI/data-parallel sub-set mode
            // First determine it for all nodes, then pick the min over all nodes, as to give all the same #frames for better load balancing.
            // TODO: No, return all; and leave it to caller to redistribute them [Zhijie Yan]
            std::vector<size_t> subsetsizes(numsubsets, 0);
            for (size_t i = 0; i < mbframes; i++)   // i is input frame index; j < i in case of MPI/data-parallel sub-set mode
            {
                const size_t framepos = (globalts + i) % _totalframes;  // (for comments, see main loop below)
                //const sequenceref & frameref = randomizedframerefs[framepos];
                const randomizer::sequenceref & frameref = rand->getSequenceRef(framepos);
                subsetsizes[frameref.chunkindex % numsubsets]++;
            }
            size_t j = subsetsizes[subsetnum];        // return what we have  --TODO: we can remove the above full computation again now
            const size_t allocframes = max(j, (mbframes + numsubsets - 1) / numsubsets);  // we leave space for the desired #frames, assuming caller will try to pad them later

            // resize feat and uids
            feat.resize(vdim.size());
            uids.resize(classids.size());
            assert(feat.size()==vdim.size());
            assert(feat.size()==numStreams);
            foreach_index(i, feat)
            {
                feat[i].resize(vdim[i], allocframes);
                feat[i].shrink(vdim[i], j);
            }

            foreach_index(k, uids)
            {
                if (issupervised())             // empty means unsupervised training -> return empty uids
                    uids[k].resize(j);
                else
                    uids[k].clear();
                latticepairs.clear();               // will push_back() below
                transcripts.clear();
            }

            // return randomized frames for the time range of those utterances
            size_t currmpinodeframecount = 0;
            for (size_t j = 0; j < mbframes; j++)
            {
                if (currmpinodeframecount >= feat[0].cols())               // MPI/data-parallel mode: all nodes return the same #frames, which is how feat(,) is allocated
                    break;

                // map to time index inside arrays
                const size_t framepos = (globalts + j) % _totalframes;  // using mod because we may actually run beyond the sweep for the last call
                //const sequenceref & frameref = randomizedframerefs[framepos];
                const randomizer::sequenceref & frameref = rand->getSequenceRef(framepos);

                // in MPI/data-parallel mode, skip frames that are not in chunks loaded for this MPI node
                if ((frameref.chunkindex % numsubsets) != subsetnum)
                    continue;

                // random utterance
                readfromdisk |= requirerandomizedchunk (frameref.chunkindex, windowbegin, windowend);    // (this is just a check; should not actually page in anything)

                for (size_t i = 0; i < numStreams; i++)
                {
                    const auto & chunkdata = rand->getChunkData(i, frameref.chunkindex);
                    auto uttframes = chunkdata.getutteranceframes (frameref.utteranceindex);
                    matrixasvectorofvectors uttframevectors (uttframes);    // (wrapper that allows m[.].size() and m[.][.] as required by augmentneighbors())
                    const size_t n = uttframevectors.size();
                    assert (n == uttframes.cols() && chunkdata.numframes (frameref.utteranceindex) == n); n;

                    // copy frame and class labels
                    const size_t t = frameref.frameindex;

                    size_t leftextent, rightextent;
                    // page in the needed range of frames
                    if (leftcontext[i] == 0 && rightcontext[i] == 0)
                    {
                        leftextent = rightextent = augmentationextent(uttframevectors[t].size(), vdim[i]);
                    }
                    else
                    {
                        leftextent = leftcontext[i];
                        rightextent = rightcontext[i];
                    }
                    augmentneighbors(uttframevectors, noboundaryflags, t, leftextent, rightextent, feat[i], currmpinodeframecount);

                    if (issupervised() && i == 0)
                    {
                        auto frameclassids = getclassids(frameref);
                        foreach_index(k, uids)
                            uids[k][currmpinodeframecount] = frameclassids[k][t];
                    }
                }

                currmpinodeframecount++;
            }
        }
        timegetbatch = timergetbatch;

        // this is the number of frames we actually moved ahead in time
        framesadvanced = mbframes;

        return readfromdisk;
    }

    bool supportsbatchsubsetting() const override
    {
        return true;
    }

    bool getbatch(const size_t globalts,
                  const size_t framesrequested, std::vector<msra::dbn::matrix> & feat, std::vector<std::vector<size_t>> & uids,
                  std::vector<const_array_ref<msra::lattices::lattice::htkmlfwordsequence::word>> & transcripts,
                  std::vector<shared_ptr<const latticesource::latticepair>> & lattices, std::vector<std::vector<size_t>> & sentendmark,
                  std::vector<std::vector<size_t>> & phoneboundaries)
    {
        size_t dummy;
        return getbatch(globalts, framesrequested, 0, 1, dummy, feat, uids, transcripts, lattices,sentendmark,phoneboundaries);
    }

    double gettimegetbatch() { return timegetbatch;}

    // alternate (updated) definition for multiple inputs/outputs - read as a vector of feature matrixes or a vector of label strings
    bool getbatch (const size_t /*globalts*/,
                   const size_t /*framesrequested*/, msra::dbn::matrix & /*feat*/, std::vector<size_t> & /*uids*/,
                   std::vector<const_array_ref<msra::lattices::lattice::htkmlfwordsequence::word>> & /*transcripts*/,
                   std::vector<shared_ptr<const latticesource::latticepair>> & /*latticepairs*/)
    {
        // should never get here
        RuntimeError("minibatchframesourcemulti: getbatch() being called for single input feature and single output feature, should use minibatchutterancesource instead\n");

        // for single input/output set size to be 1 and run old getbatch
        //feat.resize(1);
        //uids.resize(1);
        //return getbatch(globalts, framesrequested, feat[0], uids[0], transcripts, latticepairs);
    }

    size_t totalframes() const { return _totalframes; }

    // return first valid globalts to ask getbatch() for
    // In utterance mode, the epoch start may fall in the middle of an utterance.
    // We return the end time of that utterance (which, in pathological cases, may in turn be outside the epoch; handle that).
    /*implement*/ size_t firstvalidglobalts (const size_t globalts) // TODO can be const
    {
        // update randomization if a new sweep is entered
        const size_t sweep = rand->lazyrandomization (globalts, allchunks);

        // frame mode: start at sweep boundary directly // TODO so globalts needs to be at sweep boundary?
        if (framemode)
            return globalts;
        // utterance mode
        assert (globalts >= sweep * _totalframes && globalts < (sweep + 1) * _totalframes); sweep;
        // TODO use std::find
        size_t pos;
        for (pos = 0; pos < rand->getNumSequences(); pos++)
            if (rand->getSequenceRef(pos).globalts >= globalts)
                return rand->getSequenceRef(pos).globalts;   // exact or inexact match
        return rand->getSequenceRef(pos - 1).globalte();     // boundary case: requested time falls within the last utterance
    }

    const std::vector<size_t> & unitcounts() const { return counts[0]; }
    const std::vector<size_t> & unitcounts(size_t index) const { return counts[index]; }

};

};};
