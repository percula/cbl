//
//  c4ExpiryEnumerator.c
//  CBForest
//
//  Created by Jim Borden on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "c4Impl.hh"
#include "c4ExpiryEnumerator.h"

#include "DocEnumerator.hh"
#include "KeyStore.hh"
#include "varint.hh"
#include "stdint.h"

#ifdef _MSC_VER
#include <ctime>
#endif
using namespace cbforest;

struct C4ExpiryEnumerator
{
public:
    C4ExpiryEnumerator(C4Database *database) :
    _db(database),
    _e(_db->getKeyStore("expiry"), slice::null, slice::null),
    _reader(slice::null)
    {
        _endTimestamp = time(NULL);
        reset();
    }
    
    bool next() {
        if(!_e.next()) {
            return false;
        }
        
        _reader = CollatableReader(_e.doc().key());
        _reader.skipTag();
        _reader.readInt();
        _current = _reader.readString();
        
        return true;
    }
    
    slice docID() const
    {
        return _current;
    }
    
    slice key() const
    {
        return _e.doc().key();
    }
    
    void reset()
    {
        CollatableBuilder c;
        c.beginArray();
        c << (double)_endTimestamp;
        c.beginMap();
        c.endMap();
        c.endArray();
        _e = DocEnumerator(_db->getKeyStore("expiry"), slice::null, c.data());
        _reader = CollatableReader(slice::null);
    }

    void close()
    {
        _e.close();
    }
    
    C4Database *getDatabase() const
    {
        return _db;
    }
    
private:
    C4Database *_db;
    DocEnumerator _e;
    alloc_slice _current;
    CollatableReader _reader;
    uint64_t _endTimestamp;
};

C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *database, C4Error *outError)
{
    try {
        WITH_LOCK(database);
        return new C4ExpiryEnumerator(database);
    } catchError(outError);

    return NULL;
}

bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError)
{
    try {
        return e->next();
    } catchError(outError);
    
    return false;
}

C4Slice c4exp_getDocID(C4ExpiryEnumerator *e)
{
    return e->docID();
}

bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError)
{
    try {
        c4db_beginTransaction(e->getDatabase(), NULL);
        e->reset();
        Transaction *t = e->getDatabase()->transaction();
        KeyStore& expiry = e->getDatabase()->getKeyStore("expiry");
        KeyStoreWriter writer = (*t)(expiry);
        while(e->next()) {
            writer.del(e->key());
            writer.del(e->docID());
        }
        
        c4db_endTransaction(e->getDatabase(), true, NULL);
        return true;
    } catchError(outError);
    
    c4db_endTransaction(e->getDatabase(), false,  NULL);
    return false;
}

void c4exp_close(C4ExpiryEnumerator *e)
{
    if (e) {
        e->close();
    }
}

void c4exp_free(C4ExpiryEnumerator *e)
{
    delete e;
}