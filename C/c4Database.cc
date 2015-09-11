//
//  c4Database.cc
//  CBForest
//
//  Created by Jens Alfke on 9/8/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "slice.hh"
typedef forestdb::slice C4Slice;
typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;

#define kC4SliceNull forestdb::slice::null

#define C4_IMPL
#include "c4Database.h"
#undef C4_IMPL

#include "Database.hh"
#include "Document.hh"
#include "DocEnumerator.hh"
#include "LogInternal.hh"
#include "VersionedDocument.hh"
#include <assert.h>

using namespace forestdb;


// Size of ForestDB buffer cache allocated for a database
#define kDBBufferCacheSize (8*1024*1024)

// ForestDB Write-Ahead Log size (# of records)
#define kDBWALThreshold 1024

// How often ForestDB should check whether databases need auto-compaction
#define kAutoCompactInterval (5*60.0)


static void recordHTTPError(int httpStatus, C4Error* outError) {
    if (outError) {
        outError->domain = C4Error::HTTPDomain;
        outError->code = httpStatus;
    }
}

static void recordError(error e, C4Error* outError) {
    if (outError) {
        outError->domain = C4Error::ForestDBDomain;
        outError->code = e.status;
    }
}

static void recordUnknownException(C4Error* outError) {
    Warn("Unexpected C++ exception thrown from CBForest");
    if (outError) {
        outError->domain = C4Error::C4Domain;
        outError->code = 2;
    }
}


#define catchError(OUTERR) \
    catch (error err) { \
        recordError(err, OUTERR); \
    } catch (...) { \
        recordUnknownException(OUTERR); \
    }


void c4slice_free(C4Slice slice) {
    slice.free();
}


#pragma mark - DATABASES:


struct c4Database {
    Database* _db;

    c4Database()    :_db(NULL), _transaction(NULL), _transactionLevel(0) { }
    ~c4Database()   {assert(_transactionLevel == 0); delete _db;}

    void beginTransaction() {
        if (++_transactionLevel == 1)
            _transaction = new Transaction(_db);
    }

    Transaction* transaction() {
        assert(_transaction);
        return _transaction;
    }

    bool inTransaction() { return _transactionLevel > 0; }

    void endTransaction(bool commit = true) {
        assert(_transactionLevel > 0);
        if (--_transactionLevel == 0) {
            auto t = _transaction;
            _transaction = NULL;
            if (!commit)
                t->abort();
            delete t; // this commits/aborts the transaction
        }
    }

private:
    Transaction* _transaction;
    int _transactionLevel;
};


C4Database* c4db_open(C4Slice path,
                      bool readOnly,
                      C4Error *outError)
{
    auto config = Database::defaultConfig();
    config.flags = readOnly ? FDB_OPEN_FLAG_RDONLY : FDB_OPEN_FLAG_CREATE;
    config.buffercache_size = kDBBufferCacheSize;
    config.wal_threshold = kDBWALThreshold;
    config.wal_flush_before_commit = true;
    config.seqtree_opt = true;
    config.compress_document_body = true;
    config.compactor_sleep_duration = (uint64_t)kAutoCompactInterval;

    auto c4db = new c4Database;
    try {
        c4db->_db = new Database((std::string)path, config);
        return c4db;
    } catchError(outError);
    delete c4db;
    return NULL;
}


void c4db_close(C4Database* database) {
    delete database;
}


uint64_t c4db_getDocumentCount(C4Database* database) {
    try {
        auto opts = DocEnumerator::Options::kDefault;
        opts.contentOptions = Database::kMetaOnly;

        uint64_t count = 0;
        for (DocEnumerator e(*database->_db, forestdb::slice::null, forestdb::slice::null, opts);
                e.next(); ) {
            VersionedDocument vdoc(*database->_db, *e);
            if (!vdoc.isDeleted())
                ++count;
        }
        return count;
    } catchError(NULL);
    return 0;
}


C4SequenceNumber c4db_getLastSequence(C4Database* database) {
    return database->_db->lastSequence();
}


bool c4db_isInTransaction(C4Database* database) {
    return database->inTransaction();
}

bool c4db_beginTransaction(C4Database* database,
                           C4Error *outError)
{
    try {
        database->beginTransaction();
        return true;
    } catchError(outError);
    return false;
}

bool c4db_endTransaction(C4Database* database,
                         bool commit,
                         C4Error *outError)
{
    try {
        database->endTransaction(commit);
        return true;
    } catchError(outError);
    return false;
}


#pragma mark - RAW DOCUMENTS:


void c4raw_free(C4RawDocument* rawDoc) {
    if (rawDoc) {
        c4slice_free(rawDoc->key);
        c4slice_free(rawDoc->meta);
        c4slice_free(rawDoc->body);
        delete rawDoc;
    }
}


C4RawDocument* c4raw_get(C4Database* database,
                         C4Slice storeName,
                         C4Slice key,
                         C4Error *outError)
{
    try {
        KeyStore localDocs(database->_db, (std::string)storeName);
        Document doc = localDocs.get(key);
        if (!doc.exists()) {
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
            return NULL;
        }
        auto rawDoc = new C4RawDocument;
        rawDoc->key = doc.key().copy();
        rawDoc->meta = doc.meta().copy();
        rawDoc->body = doc.body().copy();
        return rawDoc;
    } catchError(outError);
    return NULL;
}


bool c4raw_put(C4Database* database,
               C4Slice storeName,
               C4Slice key,
               C4Slice meta,
               C4Slice body,
               C4Error *outError)
{
    bool abort = false;
    try {
        database->beginTransaction();
        abort = true;
        KeyStore localDocs(database->_db, (std::string)storeName);
        KeyStoreWriter localWriter = (*database->transaction())(localDocs);
        if (body.buf || meta.buf)
            localWriter.set(key, meta, body);
        else
            localWriter.del(key);
        abort = false;
        database->endTransaction();
    } catchError(outError);
    if (abort)
        database->endTransaction(false);
    return false;
}


#pragma mark - DOCUMENTS:


struct C4DocumentInternal : public C4Document {
    C4Database* _db;
    VersionedDocument _versionedDoc;
    const Revision *_selectedRev;
    alloc_slice _loadedBody;

    C4DocumentInternal(C4Database* database, C4Slice docID)
    :_db(database),
     _versionedDoc(*_db->_db, docID),
     _selectedRev(NULL)
    {
        init();
    }

    C4DocumentInternal(C4Database *database, const Document &doc)
    :_db(database),
     _versionedDoc(*_db->_db, doc),
     _selectedRev(NULL)
    {
        init();
    }

    void init() {
        docID = _versionedDoc.docID();
        revID = _versionedDoc.revID();
        flags = (C4DocumentFlags)_versionedDoc.flags();
        if (_versionedDoc.exists())
            flags = (C4DocumentFlags)(flags | kExists);
        selectRevision(_versionedDoc.currentRevision());
    }

    bool selectRevision(const Revision *rev, C4Error *outError =NULL) {
        _selectedRev = rev;
        _loadedBody = slice::null;
        if (rev) {
            selectedRev.revID = rev->revID;
            selectedRev.flags = (C4RevisionFlags)rev->flags;
            selectedRev.sequence = rev->sequence;
            selectedRev.body = rev->inlineBody();
            return true;
        } else {
            selectedRev.revID = slice::null;
            selectedRev.flags = (C4RevisionFlags)0;
            selectedRev.sequence = 0;
            selectedRev.body = slice::null;
            recordHTTPError(404, outError);
            return false;
        }
    }

    bool loadBody(C4Error *outError) {
        if (!_selectedRev)
            return false;
        if (selectedRev.body.buf)
            return true;  // already loaded
        try {
            _loadedBody = _selectedRev->readBody();
            selectedRev.body = _loadedBody;
            if (_loadedBody.buf)
                return true;
            recordHTTPError(410, outError); // 410 Gone to denote body that's been compacted away
        } catchError(outError);
        return false;
    }

    void updateMeta() {
        _versionedDoc.updateMeta();
        flags = (C4DocumentFlags)(_versionedDoc.flags() | kExists);
        revID = _versionedDoc.revID();
    }
};

static inline C4DocumentInternal *internal(C4Document *doc) {
    return (C4DocumentInternal*)doc;
}


void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        auto doc = new C4DocumentInternal(database, docID);
        if (mustExist && !doc->_versionedDoc.exists()) {
            delete doc;
            doc = NULL;
            recordError(FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    auto idoc = internal(doc);
    if (revID.buf) {
        const Revision *rev = idoc->_versionedDoc[revidBuffer(revID)];
        return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadBody(outError));
    } else {
        idoc->selectRevision(NULL);
        return true;
    }
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    auto idoc = internal(doc);
    const Revision *rev = idoc->_versionedDoc.currentRevision();
    return idoc->selectRevision(rev);
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    return internal(doc)->loadBody(outError);
}


bool c4doc_selectParentRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->parent());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextRevision(C4Document* doc) {
    auto idoc = internal(doc);
    if (idoc->_selectedRev)
        idoc->selectRevision(idoc->_selectedRev->next());
    return idoc->_selectedRev != NULL;
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    auto idoc = internal(doc);
    auto rev = idoc->_selectedRev;
    do {
        rev = rev->next();
    } while (rev && (!rev->isLeaf() || (!includeDeleted && rev->isDeleted())));
    return idoc->selectRevision(rev, outError) && (!withBody || idoc->loadBody(outError));
}


#pragma mark - INSERTING REVISIONS


bool c4doc_insertRevision(C4Document *doc,
                          C4Slice revID,
                          C4Slice body,
                          bool deleted,
                          bool hasAttachments,
                          bool allowConflict,
                          C4Error *outError)
{
    auto idoc = internal(doc);
    assert(idoc->_db->inTransaction());
    try {
        int httpStatus;
        auto newRev = idoc->_versionedDoc.insert(revidBuffer(revID),
                                                 body,
                                                 deleted,
                                                 hasAttachments,
                                                 idoc->_selectedRev,
                                                 allowConflict,
                                                 httpStatus);
        if (newRev) {
            idoc->updateMeta();
            return idoc->selectRevision(newRev);
        }
        recordHTTPError(httpStatus, outError);
    } catchError(outError)
    return false;
}


int c4doc_insertRevisionWithHistory(C4Document *doc,
                                    C4Slice revID,
                                    C4Slice body,
                                    bool deleted,
                                    bool hasAttachments,
                                    C4Slice history[],
                                    unsigned historyCount,
                                    C4Error *outError)
{
    auto idoc = internal(doc);
    assert(idoc->_db->inTransaction());
    int commonAncestor = -1;
    try {
        std::vector<revidBuffer> revIDBuffers;
        std::vector<revid> revIDs;
        revIDs.push_back(revidBuffer(revID));
        for (unsigned i = 0; i < historyCount; i++) {
            revIDBuffers.push_back(revidBuffer(history[i]));
            revIDs.push_back(revIDBuffers.back());
        }
        commonAncestor = idoc->_versionedDoc.insertHistory(revIDs,
                                                           body,
                                                           deleted,
                                                           hasAttachments);
        if (commonAncestor >= 0) {
            idoc->updateMeta();
            idoc->selectRevision(idoc->_versionedDoc[revidBuffer(revID)]);
        } else {
            recordHTTPError(400, outError); // must be invalid revision IDs
        }
    } catchError(outError)
    return commonAncestor;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = internal(doc)->_versionedDoc.docType().copy();
    return (C4SliceResult){result.buf, result.size};
}

void c4doc_setType(C4Document *doc, C4Slice docType) {
    auto idoc = internal(doc);
    assert(idoc->_db->inTransaction());
    idoc->_versionedDoc.setDocType(docType);
}


bool c4doc_save(C4Document *doc,
                unsigned maxRevTreeDepth,
                C4Error *outError)
{
    auto idoc = internal(doc);
    assert(idoc->_db->inTransaction());
    try {
        idoc->_versionedDoc.prune(maxRevTreeDepth);
        idoc->_versionedDoc.save(*idoc->_db->transaction());
        return true;
    } catchError(outError)
    return false;
}


#pragma mark - DOC ENUMERATION:


struct C4DocEnumerator {
    C4Database *_database;
    DocEnumerator _e;

    C4DocEnumerator(C4Database *database,
                    sequence start,
                    sequence end,
                    const DocEnumerator::Options& options)
    :_database(database),
     _e(*database->_db, start, end, options)
    { }

    C4DocEnumerator(C4Database *database,
                    C4Slice startDocID,
                    C4Slice endDocID,
                    const DocEnumerator::Options& options)
    :_database(database),
     _e(*database->_db, startDocID, endDocID, options)
    { }
};


void c4enum_free(C4DocEnumerator *e) {
    delete e;
}


C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                       C4SequenceNumber since,
                                       bool withBodies,
                                       C4Error *outError)
{
    auto options = DocEnumerator::Options::kDefault;
    options.inclusiveEnd = true;
    options.includeDeleted = false;
    if (!withBodies)
        options.contentOptions = KeyStore::kMetaOnly;
    return new C4DocEnumerator(database, since+1, UINT64_MAX, options);
}


C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                       C4Slice startDocID,
                                       C4Slice endDocID,
                                       bool descending,
                                       bool inclusiveEnd,
                                       unsigned skip,
                                       bool withBodies,
                                       C4Error *outError)
{
    auto options = DocEnumerator::Options::kDefault;
    options.skip = skip;
    options.descending = descending;
    options.inclusiveEnd = inclusiveEnd;
    if (!withBodies)
        options.contentOptions = KeyStore::kMetaOnly;
    return new C4DocEnumerator(database, startDocID, endDocID, options);
}


C4Document* c4enum_nextDocument(C4DocEnumerator *e, C4Error *outError) {
    try {
        if (e->_e.next())
            return new C4DocumentInternal(e->_database, e->_e.doc());
        recordError(FDB_RESULT_SUCCESS, outError);      // end of iteration is not an error
    } catchError(outError)
    return NULL;
}
