//
//  c4View.cc
//  CBForest
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4View.h"
#include "c4Document.h"
#include "c4DocEnumerator.h"
#include "Collatable.hh"
#include "MapReduceIndex.hh"
#include "FullTextIndex.hh"
#include "GeoIndex.hh"
#include "VersionedDocument.hh"
#include "Tokenizer.hh"
#include <math.h>
#include <limits.h>
using namespace cbforest;


// ForestDB Write-Ahead Log size (# of records)
static const size_t kViewDBWALThreshold = 1024;


// C4KeyReader is really identical to CollatableReader, which itself consists of nothing but
// a slice.
static inline C4KeyReader asKeyReader(const CollatableReader &r) {
    return *(C4KeyReader*)&r;
}


#pragma mark - VIEWS:


struct c4View {
    c4View(C4Database *sourceDB,
           C4Slice path,
           C4Slice name,
           const Database::config &config,
           C4Slice version)
    :_sourceDB(sourceDB),
     _viewDB((std::string)path, config),
     _index(&_viewDB, (std::string)name, sourceDB->defaultKeyStore())
    {
        Transaction t(&_viewDB);
        _index.setup(t, -1, NULL, (std::string)version);
    }

    C4Database *_sourceDB;
    Database _viewDB;
    MapReduceIndex _index;
#if C4DB_THREADSAFE
    std::mutex _mutex;
#endif
};


C4View* c4view_open(C4Database* db,
                    C4Slice path,
                    C4Slice viewName,
                    C4Slice version,
                    C4DatabaseFlags flags,
                    const C4EncryptionKey *key,
                    C4Error *outError)
{
    try {
        auto config = c4DbConfig(flags, key);
        config.wal_threshold = kViewDBWALThreshold;
        config.seqtree_opt = FDB_SEQTREE_NOT_USE; // indexes don't need by-sequence ordering

        return new c4View(db, path, viewName, config, version);
    } catchError(outError);
    return NULL;
}

/** Closes the view and frees the object. */
bool c4view_close(C4View* view, C4Error *outError) {
    try {
        delete view;
        return true;
    } catchError(outError);
    return false;
}

bool c4view_rekey(C4View *view, const C4EncryptionKey *newKey, C4Error *outError) {
    WITH_LOCK(view);
    return rekey(&view->_viewDB, newKey, outError);
}

bool c4view_eraseIndex(C4View *view, C4Error *outError) {
    try {
        WITH_LOCK(view);
        Transaction t(&view->_viewDB);
        view->_index.erase(t);
        return true;
    } catchError(outError);
    return false;
}

bool c4view_delete(C4View *view, C4Error *outError) {
    try {
		if (view == NULL) {
			return true;
		}

        WITH_LOCK(view);
        view->_viewDB.deleteDatabase();
        delete view;
        return true;
    } catchError(outError)
    return false;
}


uint64_t c4view_getTotalRows(C4View *view) {
    try {
        WITH_LOCK(view);
        return view->_index.rowCount();
    } catchError(NULL);
    return 0;
}

C4SequenceNumber c4view_getLastSequenceIndexed(C4View *view) {
    try {
        WITH_LOCK(view);
        return view->_index.lastSequenceIndexed();
    } catchError(NULL);
    return 0;
}

C4SequenceNumber c4view_getLastSequenceChangedAt(C4View *view) {
    try {
        WITH_LOCK(view);
        return view->_index.lastSequenceChangedAt();
    } catchError(NULL);
    return 0;
}


#pragma mark - INDEXING:


static void initTokenizer() {
    static bool sInitializedTokenizer = false;
    if (!sInitializedTokenizer) {
        Tokenizer::defaultStemmer = "english";
        Tokenizer::defaultRemoveDiacritics = true;
        sInitializedTokenizer = true;
    }
}


struct c4Indexer : public MapReduceIndexer {
    c4Indexer(C4Database *db)
    :MapReduceIndexer(),
     _db(db)
    {
        initTokenizer();
    }

    virtual ~c4Indexer() { }

    C4Database* _db;
};


C4Indexer* c4indexer_begin(C4Database *db,
                           C4View *views[],
                           size_t viewCount,
                           C4Error *outError)
{
    c4Indexer *indexer = NULL;
    try {
        indexer = new c4Indexer(db);
        for (size_t i = 0; i < viewCount; ++i) {
            auto t = new Transaction(&views[i]->_viewDB);
            indexer->addIndex(&views[i]->_index, t);
        }
        return indexer;
    } catchError(outError);
    if (indexer)
        delete indexer;
    return NULL;
}


void c4indexer_triggerOnView(C4Indexer *indexer, C4View *view) {
    indexer->triggerOnIndex(&view->_index);
}



C4DocEnumerator* c4indexer_enumerateDocuments(C4Indexer *indexer, C4Error *outError) {
    try {
        sequence startSequence = indexer->startingSequence();
        if (startSequence == UINT64_MAX) {
            clearError(outError);      // end of iteration is not an error
            return NULL;
        }
        auto options = kC4DefaultEnumeratorOptions;
        options.flags |= kC4IncludeDeleted;
        return c4db_enumerateChanges(indexer->_db, startSequence-1, &options, outError);
    } catchError(outError);
    return NULL;
}


bool c4indexer_shouldIndexDocument(C4Indexer *indexer,
                                   unsigned viewNumber,
                                   C4Document *doc)
{
    return indexer->shouldMapDocIntoView(versionedDocument(doc).document(), viewNumber);
}


bool c4indexer_emit(C4Indexer *indexer,
                    C4Document *doc,
                    unsigned viewNumber,
                    unsigned emitCount,
                    C4Key* const emittedKeys[],
                    C4Slice const emittedValues[],
                    C4Error *outError)
{
    C4KeyValueList kv;
    kv.keys.reserve(emitCount);
    kv.values.reserve(emitCount);
    for (unsigned i = 0; i < emitCount; ++i) {
        c4kv_add(&kv, emittedKeys[i], emittedValues[i]);
    }
    return c4indexer_emitList(indexer, doc, viewNumber, &kv, outError);
}


bool c4indexer_emitList(C4Indexer *indexer,
                    C4Document *doc,
                    unsigned viewNumber,
                    C4KeyValueList *kv,
                    C4Error *outError)
{
    try {
        if (doc->flags & kDeleted)
            c4kv_reset(kv);
        indexer->emitDocIntoView(doc->docID, doc->sequence, viewNumber, kv->keys, kv->values);
        return true;
    } catchError(outError)
    return false;
}


bool c4indexer_end(C4Indexer *indexer, bool commit, C4Error *outError) {
    try {
        if (commit)
            indexer->finished();
        delete indexer;
        return true;
    } catchError(outError)
    return false;
}


#pragma mark - QUERIES:


CBFOREST_API const C4QueryOptions kC4DefaultQueryOptions = {
    0,
    UINT_MAX,
    false,
    true,
    true,
    true
};

static DocEnumerator::Options convertOptions(const C4QueryOptions *c4options) {
    if (!c4options)
        c4options = &kC4DefaultQueryOptions;
    DocEnumerator::Options options = DocEnumerator::Options::kDefault;
    options.skip = (unsigned)c4options->skip;
    options.limit = (unsigned)c4options->limit;
    options.descending = c4options->descending;
    options.inclusiveStart = c4options->inclusiveStart;
    options.inclusiveEnd = c4options->inclusiveEnd;
    return options;
}


struct C4QueryEnumInternal : public C4QueryEnumerator {
    C4QueryEnumInternal() {
        ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));   // zero public fields
    }

    virtual ~C4QueryEnumInternal()  { }

    virtual bool next() {
        ::memset((C4QueryEnumerator*)this, 0, sizeof(C4QueryEnumerator));   // zero public fields
        return false;
    }
};

static C4QueryEnumInternal* asInternal(C4QueryEnumerator *e) {return (C4QueryEnumInternal*)e;}


bool c4queryenum_next(C4QueryEnumerator *e,
                      C4Error *outError)
{
    try {
        if (asInternal(e)->next())
            return true;
        clearError(outError);      // end of iteration is not an error
    } catchError(outError);
    return false;
}


void c4queryenum_free(C4QueryEnumerator *e) {
    delete asInternal(e);
}


#pragma mark MAP/REDUCE QUERIES:


struct C4MapReduceEnumerator : public C4QueryEnumInternal {
    C4MapReduceEnumerator(C4View *view,
                        Collatable startKey, slice startKeyDocID,
                        Collatable endKey, slice endKeyDocID,
                        const DocEnumerator::Options &options)
    :_enum(&view->_index, startKey, startKeyDocID, endKey, endKeyDocID, options)
    { }

    C4MapReduceEnumerator(C4View *view,
                        std::vector<KeyRange> keyRanges,
                        const DocEnumerator::Options &options)
    :_enum(&view->_index, keyRanges, options)
    { }

    virtual bool next() {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        key = asKeyReader(_enum.key());
        value = _enum.value();
        docID = _enum.docID();
        docSequence = _enum.sequence();
        return true;
    }

private:
    IndexEnumerator _enum;
};


C4QueryEnumerator* c4view_query(C4View *view,
                                const C4QueryOptions *c4options,
                                C4Error *outError)
{
    try {
        if (!c4options)
            c4options = &kC4DefaultQueryOptions;
        DocEnumerator::Options options = convertOptions(c4options);

        if (c4options->keysCount == 0 && c4options->keys == NULL) {
            Collatable noKey;
            return new C4MapReduceEnumerator(view,
                                           (c4options->startKey ? (Collatable)*c4options->startKey
                                                                : noKey),
                                           c4options->startKeyDocID,
                                           (c4options->endKey ? (Collatable)*c4options->endKey
                                                              : noKey),
                                           c4options->endKeyDocID,
                                           options);
        } else {
            std::vector<KeyRange> keyRanges;
            for (size_t i = 0; i < c4options->keysCount; i++) {
                const C4Key* key = c4options->keys[i];
                if (key)
                    keyRanges.push_back(KeyRange(*key));
            }
            return new C4MapReduceEnumerator(view, keyRanges, options);
        }
    } catchError(outError);
    return NULL;
}


#pragma mark FULL-TEXT QUERIES:


struct C4FullTextEnumerator : public C4QueryEnumInternal {
    C4FullTextEnumerator(C4View *view,
                         slice queryString,
                         slice queryStringLanguage,
                         bool ranked,
                         const DocEnumerator::Options &options)
    :_enum(&view->_index, queryString, queryStringLanguage, ranked, options)
    { }

    virtual bool next() {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        auto match = _enum.match();
        docID = match->docID;
        docSequence = match->sequence;
        _allocatedValue = match->value();
        value = _allocatedValue;
        fullTextID = match->fullTextID();
        fullTextTermCount = (uint32_t)match->textMatches.size();
        fullTextTerms = (const C4FullTextTerm*)match->textMatches.data();
        return true;
    }

    alloc_slice fullTextMatched() {
        return _enum.match()->matchedText();
    }

private:
    FullTextIndexEnumerator _enum;
    alloc_slice _allocatedValue;
};


C4QueryEnumerator* c4view_fullTextQuery(C4View *view,
                                        C4Slice queryString,
                                        C4Slice queryStringLanguage,
                                        const C4QueryOptions *c4options,
                                        C4Error *outError)
{
    try {
        return new C4FullTextEnumerator(view, queryString, queryStringLanguage,
                                        (c4options ? c4options->rankFullText : true),
                                        convertOptions(c4options));
    } catchError(outError);
    return NULL;
}


C4SliceResult c4view_fullTextMatched(C4View *view,
                                     C4Slice docID,
                                     C4SequenceNumber seq,
                                     unsigned fullTextID,
                                     C4Error *outError)
{
    try {
        auto result = FullTextMatch::matchedText(&view->_index, docID, seq, fullTextID).dontFree();
        return {result.buf, result.size};
    } catchError(outError);
    return {NULL, 0};
}


C4SliceResult c4queryenum_fullTextMatched(C4QueryEnumerator *e) {
    try {
        slice result = ((C4FullTextEnumerator*)e)->fullTextMatched().dontFree();
        return {result.buf, result.size};
    } catchError(NULL);
    return {NULL, 0};
}


bool c4key_setDefaultFullTextLanguage(C4Slice languageName, bool stripDiacriticals) {
    initTokenizer();
    Tokenizer::defaultStemmer = std::string(languageName);
    Tokenizer::defaultRemoveDiacritics = stripDiacriticals;
    return true;
}


#pragma mark GEO-QUERIES:


struct C4GeoEnumerator : public C4QueryEnumInternal {
    C4GeoEnumerator(C4View *view, const geohash::area &bbox)
    :_enum(&view->_index, bbox)
    { }

    virtual bool next() {
        if (!_enum.next())
            return C4QueryEnumInternal::next();
        docID = _enum.docID();
        docSequence = _enum.sequence();
        value = _enum.value();
        auto bbox = _enum.keyBoundingBox();
        geoBBox.xmin = bbox.min().longitude;
        geoBBox.ymin = bbox.min().latitude;
        geoBBox.xmax = bbox.max().longitude;
        geoBBox.ymax = bbox.max().latitude;
        geoJSON = _enum.keyGeoJSON();
        return true;
    }

private:
    GeoIndexEnumerator _enum;
};


C4QueryEnumerator* c4view_geoQuery(C4View *view,
                                   C4GeoArea area,
                                   C4Error *outError)
{
    try {
        geohash::area ga(geohash::coord(area.xmin, area.ymin),
                         geohash::coord(area.xmax, area.ymax));
        return new C4GeoEnumerator(view, ga);
    } catchError(outError);
    return NULL;
}
