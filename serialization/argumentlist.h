/*
   Copyright (C) 2013 Andreas Hartmetz <ahartmetz@gmail.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LGPL.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.

   Alternatively, this file is available under the Mozilla Public License
   Version 1.1.  You may obtain a copy of the License at
   http://www.mozilla.org/MPL/
*/

#ifndef ARGUMENTLIST_H
#define ARGUMENTLIST_H

#include "types.h"

#include <cassert>
#include <string>
#include <vector>

class ArgumentList
{
public:
    ArgumentList(); // constructs an empty argument list
     // constructs an argument list to deserialize data in @p data with signature @p signature
    ArgumentList(cstring signature, array data, bool isByteSwapped = false);

    // copying needs special treatment due to the d-pointer
    ArgumentList(const ArgumentList &other);
    void operator=(const ArgumentList &other);

    ~ArgumentList();

    std::string prettyPrint() const;

     // returns true when at least one read cursor is open, false otherwise
    bool isReading() const;
    // returns true when a write cursor is open, false otherwise
    bool isWriting() const;

    cstring signature() const;
    array data() const;

    class ReadCursor;
    class WriteCursor;
    ReadCursor beginRead();
    WriteCursor beginWrite();

    enum SignatureType {
        MethodSignature = 0,
        VariantSignature
    };

    static bool isStringValid(cstring string);
    static bool isObjectPathValid(cstring objectPath);
    static bool isObjectPathElementValid(cstring pathElement);
    static bool isSignatureValid(cstring signature, SignatureType type = MethodSignature);

    static const int maxSignatureLength = 255;

    enum CursorState {
        // "exceptional" states
        NotStarted = 0,
        Finished,
        NeedMoreData, // recoverable by adding data; should only happen when parsing the not length-prefixed variable message header
        InvalidData, // non-recoverable
        // WriteCursor states when the next type is still open (not iterating in an array or dict)
        AnyData, // occurs in WriteCursor when you are free to add any type
        DictKey, // occurs in WriteCursor when the next type must be suitable for a dict key -
                 // a simple string or numeric type.

        // the following occur in ReadCursor, and in WriteCursor when in the second or higher iteration
        // of an array or dict where the types must match the first iteration (except inside variants).

        // states pertaining to aggregates
        BeginArray,
        NextArrayEntry,
        EndArray,
        BeginDict,
        NextDictEntry,
        EndDict,
        BeginStruct,
        EndStruct,
        BeginVariant,
        EndVariant,
        // the next element is plain data
        Byte,
        Boolean,
        Int16,
        Uint16,
        Int32,
        Uint32,
        Int64,
        Uint64,
        Double,
        String,
        ObjectPath,
        Signature,
        UnixFd
    };

private:
    struct podCstring // Same as cstring but without ctor.
                      // Can't put the cstring type into a union because it has a constructor :/
    {
        byte *begin;
        uint32 length;
    };

    typedef union
    {
        byte Byte;
        bool Boolean;
        int16 Int16;
        uint16 Uint16;
        int32 Int32;
        uint32 Uint32;
        int64 Int64;
        uint64 Uint64;
        double Double;
        podCstring String; // also for ObjectPath and Signature
    } DataUnion;

public:

    // a cursor is similar to an iterator, but more tied to the underlying data structure
    // error handling is done by asking state() or isError(), not by method return values.
    // occasionally looking at isError() is less work than checking every call.
    class ReadCursor
    {
    public:
        ReadCursor(ReadCursor &&other);
        void operator=(ReadCursor &&other);
        ReadCursor(const ReadCursor &other) = delete;
        void operator=(const ReadCursor &other) = delete;

        ~ReadCursor();

        bool isValid() const;

        CursorState state() const { return m_state; }
        cstring stateString() const;
         // HACK call this in NeedMoreData state when more data has been added; this replaces m_data
         // ### will need to fix up any VariantInfo::prevSignature on the stack where prevSignature
         //     is inside m_data; length will still work but begin will be outdated.
        void replaceData(array data); // TODO move this to ArgumentList

        bool isFinished() const { return m_state == Finished; }
        bool isError() const { return m_state == InvalidData || m_state == NeedMoreData; }

        // when @p isEmpty is not null and the array contains no elements, the array is
        // iterated over once so you can get the type information. due to lack of data,
        // all contained arrays, dicts and variants (but not structs) will be empty, and
        // any values returned by read... will be garbage.
        // in any case, *isEmpty will be set to indicate whether the array is empty.

        void beginArray(bool *isEmpty = 0);
        // call this before reading each entry; when it returns false the array has ended.
        // TODO implement & document that all values returned by read... are zero/null?
        bool nextArrayEntry();
        void endArray(); // leaves the current array; only  call this in state EndArray!

        void beginDict(bool *isEmpty = 0);
        bool nextDictEntry(); // like nextArrayEntry()
        void endDict(); // like endArray()

        void beginStruct();
        void endStruct(); // like endArray()

        void beginVariant();
        void endVariant(); // like endArray()

        std::vector<CursorState> aggregateStack() const; // the aggregates the cursor is currently in

        // reading a type that is not indicated by state() will cause undefined behavior and at
        // least return garbage.
        byte readByte() { byte ret = m_u.Byte; advanceState(); return ret; }
        bool readBoolean() { bool ret = m_u.Boolean; advanceState(); return ret; }
        int16 readInt16() { int ret = m_u.Int16; advanceState(); return ret; }
        uint16 readUint16() { uint16 ret = m_u.Uint16; advanceState(); return ret; }
        int32 readInt32() { int32 ret = m_u.Int32; advanceState(); return ret; }
        uint32 readUint32() { uint32 ret = m_u.Uint32; advanceState(); return ret; }
        int64 readInt64() { int64 ret = m_u.Int64; advanceState(); return ret; }
        uint64 readUint64() { uint64 ret = m_u.Uint64; advanceState(); return ret; }
        double readDouble() { double ret = m_u.Double; advanceState(); return ret; }
        cstring readString() { cstring ret(m_u.String.begin, m_u.String.length); advanceState(); return ret; }
        cstring readObjectPath() { cstring ret(m_u.String.begin, m_u.String.length); advanceState(); return ret; }
        cstring readSignature() { cstring ret(m_u.String.begin, m_u.String.length); advanceState(); return ret; }
        uint32 readUnixFd() { uint32 ret = m_u.Uint32; advanceState(); return ret; }

    private:
        class Private;
        friend class Private;
        friend class ArgumentList;
        explicit ReadCursor(ArgumentList *al);
        CursorState doReadPrimitiveType();
        CursorState doReadString(int lengthPrefixSize);
        void advanceState();
        void advanceStateFrom(CursorState expectedState);
        void beginArrayOrDict(bool isDict, bool *isEmpty);
        bool nextArrayOrDictEntry(bool isDict);

        Private *d;

        // two data members not behind d-pointer for performance reasons, especially inlining
        CursorState m_state;

        // it is more efficient, in code size and performance, to read the data in advanceState()
        // and store the result for later retrieval in readFoo()
        DataUnion m_u;
    };

    // TODO: try to share code with ReadIterator
    class WriteCursor
    {
    public:
        WriteCursor(WriteCursor &&other);
        void operator=(WriteCursor &&other);
        WriteCursor(const WriteCursor &other) = delete;
        void operator=(const WriteCursor &other) = delete;

        ~WriteCursor();

        bool isValid() const;

        CursorState state() const { return m_state; }
        cstring stateString() const;

        void beginArray(bool isEmpty);
        // call this before writing each entry; calling it before the first entry is optional for
        // the convenience of client code.
        void nextArrayEntry();
        void endArray();

        void beginDict(bool isEmpty);
        void nextDictEntry();
        void endDict();

        void beginStruct();
        void endStruct();

        void beginVariant();
        void endVariant();

        void finish();

        std::vector<CursorState> aggregateStack() const; // the aggregates the cursor is currently in

        void writeByte(byte b);
        void writeBoolean(bool b);
        void writeInt16(int16 i);
        void writeUint16(uint16 i);
        void writeInt32(int32 i);
        void writeUint32(uint32 i);
        void writeInt64(int64 i);
        void writeUint64(uint64 i);
        void writeDouble(double d);
        void writeString(cstring string);
        void writeObjectPath(cstring objectPath);
        void writeSignature(cstring signature);
        void writeUnixFd(uint32 fd);

    private:
        friend class ArgumentList;
        class Private;
        friend class Private;
        explicit WriteCursor(ArgumentList *al);

        CursorState doWritePrimitiveType(uint32 alignAndSize);
        CursorState doWriteString(int lengthPrefixSize);
        void advanceState(array signatureFragment, CursorState newState);
        void beginArrayOrDict(bool isDict, bool isEmpty);
        void nextArrayOrDictEntry(bool isDict);

        Private *d;

        // two data members not behind d-pointer for performance reasons
        CursorState m_state;

        // ### check if it makes any performance difference to have this here (writeFoo() should benefit)
        DataUnion m_u;
    };

private:
    class Private;
    Private *d;
};

#endif // ARGUMENTLIST_H
