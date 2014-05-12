//
//  CBForestDB.h
//  CBForest
//
//  Created by Jens Alfke on 9/4/12.
//  Copyright (c) 2012 Couchbase, Inc. All rights reserved.
//

#import <Foundation/Foundation.h>
@class CBForestDocument;


/** NSError domain string for errors specific to CBForest; the error codes correspond to the
    fdb_status enum (see <fdb_errors.h>) except for the ones added below. */
extern NSString* const CBForestErrorDomain;

enum {
    kCBForestErrorInvalidArgs = -1,
    kCBForestErrorOpenFailed = -2,
    kCBForestErrorFileNotFound = -3,
    kCBForestErrorWriteFailed = -4,
    kCBForestErrorReadFailed = -5,
    kCBForestErrorCloseFailed = -6,
    kCBForestErrorCommitFailed = -7,
    kCBForestErrorAllocFailed = -8,
    kCBForestErrorNotFound = -9,
    kCBForestErrorReadOnly = -10,
    kCBForestErrorCompactionFailed = -11,
    kCBForestErrorIteratorFailed = -12,
    kCBForestErrorSeekFailed = -13,
    kCBForestErrorFsyncFailed = -14,
    kCBForestErrorChecksum = -15,
    kCBForestErrorFileCorrupt = -16,
    kCBForestErrorCompressionFailed = -17,
    kCBForestErrorNoDBInstance = -18,
    kCBForestErrorFailByRollback = -19,
    kCBForestErrorInvalidConfig = -20,
    kCBForestErrorNoManualCompaction = -21,

    // Errors specific to CBForest, not defined by ForestDB:
    kCBForestErrorRevisionDataCorrupt = -1000,
    kCBForestErrorTransactionAborted = -1001
};


/** Option flag bits for opening CBForest databases. */
typedef enum {
    kCBForestDBCreate       = 0x01,
    kCBForestDBReadOnly     = 0x02
} CBForestFileOptions;


typedef struct {
    uint64_t bufferCacheSize;           // Size of in-memory data cache
    uint64_t walThreshold;              // if nonzero, enables WAL flushing before commits
    BOOL enableSequenceTree;            // Should database track sequences?
    BOOL compressDocBodies;             // Should docs be compressed on-disk with Snappy?
    uint8_t autoCompactThreshold;       // Percentage of wasted space that triggers auto-compact
} CBForestDBConfig;


/** Option flag bits for loading & enumerating documents in a CBForest. */
typedef enum {
    kCBForestDBCreateDoc    = 0x01, // used only by -documentWithID:
    kCBForestDBMetaOnly     = 0x02  // used by enumerateDocs methods
} CBForestContentOptions;

typedef struct {
    unsigned                skip;
    unsigned                limit;
    BOOL                    descending;
    BOOL                    inclusiveEnd;
    BOOL                    includeDeleted;
    BOOL                    onlyConflicts;
    CBForestContentOptions  contentOptions;
} CBForestEnumerationOptions;

extern const CBForestEnumerationOptions kCBForestEnumerationOptionsDefault;


/** Sequence number type. Sequences are allocated starting from 1. */
typedef uint64_t CBForestSequence;

#define kCBForestNoSequence     ((CBForestSequence)0)   // Means "no sequence assigned/known"
#define kCBForestMaxSequence    UINT64_MAX      // Max possible sequence, for use when enumerating


/** Callback block to pass to enumeration methods. */
typedef void (^CBForestValueIterator)(NSData* key, NSData* value, NSData* meta, BOOL *stop);

/** Callback block to pass to enumeration methods. */
typedef void (^CBForestDocIterator)(CBForestDocument* doc, BOOL *stop);

/** Database statistics returned by the .info property. */
typedef struct {
    uint64_t         documentCount;
    uint64_t         dataSize;
    uint64_t         fileSize;
    CBForestSequence lastSequence;
} CBForestDBInfo;


/** An open CBForest database. */
@interface CBForestDB : NSObject

+ (void) setAutoCompactInterval: (NSTimeInterval)interval;

/** Opens a database at the given filesystem path.
    @param filePath The name of the file containing the database
    @param options Additional flags for how the database should be opened
    @param outError  On failure, will be set to the error that occurred */
- (id) initWithFile: (NSString*)filePath
            options: (CBForestFileOptions)options
             config: (const CBForestDBConfig*)config
              error: (NSError**)outError;

+ (CBForestDBConfig)defaultConfig;

/** The filesystem path the database was opened on. */
@property (readonly) NSString* filename;

/** Some stats about the database. */
@property (readonly) CBForestDBInfo info;

@property (readonly) BOOL isReadOnly;

/** Closes the database. It's not strictly necessary to call this -- the database will be closed when this object is deallocated -- but it's a good way to ensure it gets closed in a timely manner.
    It's illegal to call any other of the methods defined here after closing the database; they will all raise exceptions. */
- (void) close;

/** Closes the database and deletes its file. */
- (BOOL) deleteDatabase: (NSError**)outError;

/** Updates the database file header and makes sure all writes have been flushed to the disk.
    Until this happens, no changes made will persist: they aren't visible to any other client
    who opens the database, and will be lost if you close and re-open the database. */
- (BOOL) commit: (NSError**)outError;

/** Reverts the database to the state it was in at the given sequence number. */
- (BOOL) rollbackToSequence: (CBForestSequence)oldSequence
                      error: (NSError**)outError;

/** Opens a new database handle on this file, a read-only snapshot containing the database
    contents as of the given sequence. */
- (CBForestDB*) openSnapshotAtSequence: (CBForestSequence)sequence
                                 error: (NSError**)outError;

/** Copies current versions of all documents to a new file, then replaces the current database
    file with the new one. */
- (BOOL) compact: (NSError**)outError;

/** Erase the contents of the file. This actually closes, deletes and re-opens the database. */
- (BOOL) erase: (NSError**)outError;


// TRANSACTIONS:

- (void) beginTransaction;
- (void) failTransaction;
- (BOOL) endTransaction: (NSError**)outError;

/** Runs the block, then calls -commit:. If transactions are nested (i.e. -inTransaction: is called
    while in the block) only the outermost transaction calls -commit:.
    The block can return NO to signal failure, but this doesn't prevent the commit (since ForestDB
    doesn't currently have any way to roll-back changes); it just causes the -inTransaction: call
    to return NO as well, as a convenience to the caller.
    @return  YES if the block returned YES and the commit succeeded; else NO. */
- (BOOL) inTransaction: (BOOL(^)())block;


// KEYS/VALUES:

/** Stores a value blob for a key blob, replacing any previous value.
    Use a nil value to delete. */
- (CBForestSequence) setValue: (NSData*)value
                         meta: (NSData*)meta
                       forKey: (NSData*)key
                        error: (NSError**)outError;

/** Asynchronous store. Can only be used inside a transaction.
    The completion block is optional; it will be called on the database's internal dispatch queue. */
- (void) asyncSetValue: (NSData*)value
                  meta: (NSData*)meta
                forKey: (NSData*)key
            onComplete: (void(^)(CBForestSequence,NSError*))onComplete;

/** Loads the value blob with the given key blob, plus its metadata.
    If there is no value for the key, no error is returned, but the value and meta will be nil. */
- (BOOL) getValue: (NSData**)outValue
             meta: (NSData**)outMeta
           forKey: (NSData*)key
            error: (NSError**)outError;

- (BOOL) hasValueForKey: (NSData*)key;

/** Deletes the document/value with the given sequence. */
- (BOOL) deleteSequence: (CBForestSequence)sequence
                  error: (NSError**)outError;

- (void) asyncDeleteSequence: (CBForestSequence)sequence;

// DOCUMENTS:

/** What class of document to create (must inherit from CBForestDocument) */
@property Class documentClass;

/** Instantiates a CBForestDocument with the given document ID,
    but doesn't load its data or metadata yet.
    This method is used to create new documents; if you want to read an existing document
    it's better to call -documentWithID:error: instead. */
- (CBForestDocument*) makeDocumentWithID: (NSString*)docID;

/** Loads the document with the given ID into a CBForestDocument object. */
- (CBForestDocument*) documentWithID: (NSString*)docID
                             options: (CBForestContentOptions)options
                               error: (NSError**)outError;

/** Loads the metadata of the document with the given sequence number,
    into a CBForestDocument object. */
- (CBForestDocument*) documentWithSequence: (CBForestSequence)sequence
                                   options: (CBForestContentOptions)options
                                     error: (NSError**)outError;

- (BOOL) deleteDocument: (CBForestDocument*)doc error: (NSError**)outError;

/** Iterates over documents, in ascending order by key.
    @param startID  The document ID to start at, or nil to start from the beginning.
    @param endID  The last document ID to enumerate, or nil to go to the end.
    @param options  Iteration options, or NULL to use the default options.
    @param outError  On failure, an NSError will be stored here (unless it's NULL).
    @return  An enumerator object. */
- (NSEnumerator*) enumerateDocsFromID: (NSString*)startID
                                 toID: (NSString*)endID
                              options: (const CBForestEnumerationOptions*)options
                                error: (NSError**)outError;

/** Iterates over documents, in ascending order by sequence.
    @param startSequence  The sequence number to start at (1 to start from the beginning.)
    @param endSequence  The sequence number to end at, or kCBForestMaxSequence to go to the end.
    @param options  Iteration options, or NULL to use the default options.
    @param outError  On failure, an NSError will be stored here (unless it's NULL).
    @return  An enumerator object. */
- (NSEnumerator*) enumerateDocsFromSequence: (CBForestSequence)startSequence
                                 toSequence: (CBForestSequence)endSequence
                                    options: (const CBForestEnumerationOptions*)options
                                      error: (NSError**)outError;

/** Iterates over a documents, given an array of keys.
    @param keys  The keys (NSData*) or document-IDs (NSString*) to iterate over.
    @param options  Iteration options, or NULL to use the default options.
    @param outError  On failure, an NSError will be stored here (unless it's NULL).
    @return  An enumerator object. */
- (NSEnumerator*) enumerateDocsWithKeys: (NSArray*)keys
                                options: (const CBForestEnumerationOptions*)options
                                  error: (NSError**)outError;

/** Returns a dump of every document in the database with its metadata and body sizes. */
- (NSString*) dump;

@end
