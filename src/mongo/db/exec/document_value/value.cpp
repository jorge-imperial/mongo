/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/document_value/value.h"

#include "mongo/base/compare_numbers.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/hex.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/str.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <type_traits>
#include <typeinfo>

#include <absl/hash/hash.h>
#include <absl/strings/string_view.h>
#include <boost/cstdint.hpp>
#include <boost/functional/hash.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/numeric/conversion/converter_policies.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::numeric_limits;
using std::ostream;
using std::string;
using std::stringstream;
using std::vector;
using namespace std::string_literals;

void ValueStorage::putString(StringData s) {
    // Note: this also stores data portion of BinData
    const size_t sizeNoNUL = s.size();
    if (sizeNoNUL <= sizeof(shortStrStorage)) {
        shortStr = true;
        shortStrSize = s.size();
        s.copy(shortStrStorage, s.size());

        // All memory is zeroed before this is called, so we know that
        // the nulTerminator field will definitely contain a NUL byte.
        dassert(((sizeNoNUL < sizeof(shortStrStorage)) && (shortStrStorage[sizeNoNUL] == '\0')) ||
                (((shortStrStorage + sizeNoNUL) == &nulTerminator) && (nulTerminator == '\0')));
    } else {
        putRefCountable(RCString::create(s));
    }
}

void ValueStorage::putDocument(const Document& d) {
    putRefCountable(d._storage);
}

void ValueStorage::putDocument(Document&& d) {
    putRefCountable(std::move(d._storage));
}

void ValueStorage::putVector(boost::intrusive_ptr<RCVector<Value>>&& vec) {
    fassert(16485, bool(vec));
    putRefCountable(std::move(vec));
}

void ValueStorage::putRegEx(const BSONRegEx& re) {
    const size_t patternLen = re.pattern.size();
    const size_t flagsLen = re.flags.size();
    const size_t totalLen = patternLen + 1 /*middle NUL*/ + flagsLen;

    // Need to copy since putString doesn't support scatter-gather.
    std::unique_ptr<char[]> buf(new char[totalLen]);
    auto dest = buf.get();
    dest = str::copyAsCString(dest, re.pattern);
    re.flags.copy(dest, re.flags.size());  // NUL added automatically by putString()
    putString(StringData(buf.get(), totalLen));
}

Document ValueStorage::getDocument() const {
    if (!genericRCPtr)
        return Document();

    dassert(typeid(*genericRCPtr) == typeid(const DocumentStorage));
    const DocumentStorage* documentPtr = static_cast<const DocumentStorage*>(genericRCPtr);
    return Document(documentPtr);
}

// not in header because document is fwd declared
Value::Value(const BSONObj& obj) : _storage(Object, Document(obj.getOwned())) {}

// An option of providing 'Value(Document)' was rejected in favor of 'Value(const Document&)' and
// 'Value(Document&&)' overloads, and lvalue/rvalue reference overloads of callees, since
// 'Value(Document)' option with a lvalue parameter would result in one extra move operation in
// 'ValueStorage::putDocument()'.
Value::Value(const Document& doc) : _storage(Object, doc.isOwned() ? doc : doc.getOwned()) {}
Value::Value(Document&& doc)
    : _storage(Object, doc.isOwned() ? std::move(doc) : std::move(doc).getOwned()) {}

Value::Value(const BSONElement& elem) : _storage(elem.type()) {
    switch (elem.type()) {
        // These are all type-only, no data
        case EOO:
        case MinKey:
        case MaxKey:
        case Undefined:
        case jstNULL:
            break;

        case NumberDouble:
            _storage.doubleValue = elem.Double();
            break;

        case Code:
        case Symbol:
        case String:
            _storage.putString(elem.valueStringData());
            break;

        case Object: {
            _storage.putDocument(Document(elem.embeddedObject().getOwned()));
            break;
        }

        case Array: {
            auto vec = make_intrusive<RCVector<Value>>();
            for (auto&& sub : elem.embeddedObject()) {
                vec->vec.push_back(Value(sub));
            }
            _storage.putVector(std::move(vec));
            break;
        }

        case jstOID:
            MONGO_STATIC_ASSERT(sizeof(_storage.oid) == OID::kOIDSize);
            memcpy(_storage.oid, elem.OID().view().view(), OID::kOIDSize);
            break;

        case Bool:
            _storage.boolValue = elem.boolean();
            break;

        case Date:
            _storage.dateValue = elem.date().toMillisSinceEpoch();
            break;

        case RegEx: {
            _storage.putRegEx(BSONRegEx(elem.regex(), elem.regexFlags()));
            break;
        }

        case NumberInt:
            _storage.intValue = elem.numberInt();
            break;

        case bsonTimestamp:
            _storage.timestampValue = elem.timestamp().asULL();
            break;

        case NumberLong:
            _storage.longValue = elem.numberLong();
            break;

        case NumberDecimal:
            _storage.putDecimal(elem.numberDecimal());
            break;

        case CodeWScope: {
            StringData code(elem.codeWScopeCode(), elem.codeWScopeCodeLen() - 1);
            _storage.putCodeWScope(BSONCodeWScope(code, elem.codeWScopeObject()));
            break;
        }

        case BinData: {
            int len;
            const char* data = elem.binData(len);
            _storage.putBinData(BSONBinData(data, len, elem.binDataType()));
            break;
        }

        case DBRef:
            _storage.putDBRef(BSONDBRef(elem.dbrefNS(), elem.dbrefOID()));
            break;
    }
}

Value::Value(const BSONArray& arr) : _storage(Array) {
    auto vec = make_intrusive<RCVector<Value>>();
    for (auto&& sub : arr) {
        vec->vec.push_back(Value(sub));
    }
    _storage.putVector(std::move(vec));
}

Value::Value(const vector<BSONObj>& vec) : _storage(Array) {
    auto storageVec = make_intrusive<RCVector<Value>>();
    storageVec->vec.reserve(vec.size());
    for (auto&& obj : vec) {
        storageVec->vec.push_back(Value(obj));
    }
    _storage.putVector(std::move(storageVec));
}

Value::Value(const vector<Document>& vec) : _storage(Array) {
    auto storageVec = make_intrusive<RCVector<Value>>();
    storageVec->vec.reserve(vec.size());
    for (auto&& obj : vec) {
        storageVec->vec.push_back(Value(obj));
    }
    _storage.putVector(std::move(storageVec));
}

Value::Value(const SafeNum& value) : _storage(value.type()) {
    switch (value.type()) {
        case EOO:
            break;
        case NumberInt:
            _storage.intValue = value._value.int32Val;
            break;
        case NumberLong:
            _storage.longValue = value._value.int64Val;
            break;
        case NumberDouble:
            _storage.doubleValue = value._value.doubleVal;
            break;
        case NumberDecimal:
            _storage.putDecimal(Decimal128(value._value.decimalVal));
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

Value Value::createIntOrLong(long long longValue) {
    int intValue = longValue;
    if (intValue != longValue) {
        // it is too large to be an int and should remain a long
        return Value(longValue);
    }

    // should be an int since all arguments were int and it fits
    return Value(intValue);
}

Decimal128 Value::getDecimal() const {
    BSONType type = getType();
    if (type == NumberInt)
        return Decimal128(static_cast<int32_t>(_storage.intValue));
    if (type == NumberLong)
        return Decimal128(static_cast<int64_t>(_storage.longValue));
    if (type == NumberDouble)
        return Decimal128(_storage.doubleValue);
    invariant(type == NumberDecimal);
    return _storage.getDecimal();
}

double Value::getDouble() const {
    BSONType type = getType();
    if (type == NumberInt)
        return _storage.intValue;
    if (type == NumberLong)
        return static_cast<double>(_storage.longValue);
    if (type == NumberDecimal)
        return _storage.getDecimal().toDouble();

    MONGO_verify(type == NumberDouble);
    return _storage.doubleValue;
}

Document Value::getDocument() const {
    MONGO_verify(getType() == Object);
    return _storage.getDocument();
}

Value Value::operator[](size_t index) const {
    if (getType() != Array || index >= getArrayLength())
        return Value();

    return getArray()[index];
}

Value Value::operator[](StringData name) const {
    if (getType() != Object)
        return Value();

    return getDocument()[name];
}

BSONObjBuilder& operator<<(BSONObjBuilder::ValueStream& builder, const Value& val) {
    switch (val.getType()) {
        case EOO:
            return builder.builder();  // nothing appended
        case MinKey:
            return builder << MINKEY;
        case MaxKey:
            return builder << MAXKEY;
        case jstNULL:
            return builder << BSONNULL;
        case Undefined:
            return builder << BSONUndefined;
        case jstOID:
            return builder << val.getOid();
        case NumberInt:
            return builder << val.getInt();
        case NumberLong:
            return builder << val.getLong();
        case NumberDouble:
            return builder << val.getDouble();
        case NumberDecimal:
            return builder << val.getDecimal();
        case String:
            return builder << val.getStringData();
        case Bool:
            return builder << val.getBool();
        case Date:
            return builder << val.getDate();
        case bsonTimestamp:
            return builder << val.getTimestamp();
        case Object:
            return builder << val.getDocument();
        case Symbol:
            return builder << BSONSymbol(val.getRawData());
        case Code:
            return builder << BSONCode(val.getRawData());
        case RegEx:
            return builder << BSONRegEx(val.getRegex(), val.getRegexFlags());

        case DBRef:
            return builder << BSONDBRef(val._storage.getDBRef()->ns, val._storage.getDBRef()->oid);

        case BinData:
            return builder << BSONBinData(val.getRawData().data(),  // looking for void*
                                          val.getRawData().size(),
                                          val._storage.binDataType());

        case CodeWScope:
            return builder << BSONCodeWScope(val._storage.getCodeWScope()->code,
                                             val._storage.getCodeWScope()->scope);

        case Array: {
            BSONArrayBuilder arrayBuilder(builder.subarrayStart());
            for (auto&& value : val.getArray()) {
                value.addToBsonArray(&arrayBuilder);
            }
            arrayBuilder.doneFast();
            return builder.builder();
        }
    }
    MONGO_verify(false);
}

void Value::addToBsonObj(BSONObjBuilder* builder,
                         StringData fieldName,
                         size_t recursionLevel) const {
    uassert(ErrorCodes::Overflow,
            str::stream() << "cannot convert document to BSON because it exceeds the limit of "
                          << BSONDepth::getMaxAllowableDepth() << " levels of nesting",
            recursionLevel <= BSONDepth::getMaxAllowableDepth());

    if (getType() == BSONType::Object) {
        BSONObjBuilder subobjBuilder(builder->subobjStart(fieldName));
        getDocument().toBson(&subobjBuilder, recursionLevel + 1);
        subobjBuilder.doneFast();
    } else if (getType() == BSONType::Array) {
        BSONArrayBuilder subarrBuilder(builder->subarrayStart(fieldName));
        for (auto&& value : getArray()) {
            value.addToBsonArray(&subarrBuilder, recursionLevel + 1);
        }
        subarrBuilder.doneFast();
    } else {
        *builder << fieldName << *this;
    }
}

void Value::addToBsonArray(BSONArrayBuilder* builder, size_t recursionLevel) const {
    uassert(ErrorCodes::Overflow,
            str::stream() << "cannot convert document to BSON because it exceeds the limit of "
                          << BSONDepth::getMaxAllowableDepth() << " levels of nesting",
            recursionLevel <= BSONDepth::getMaxAllowableDepth());

    // If this Value is empty, do nothing to avoid incrementing the builder's counter.
    if (missing()) {
        return;
    }

    if (getType() == BSONType::Object) {
        BSONObjBuilder subobjBuilder(builder->subobjStart());
        getDocument().toBson(&subobjBuilder, recursionLevel + 1);
        subobjBuilder.doneFast();
    } else if (getType() == BSONType::Array) {
        BSONArrayBuilder subarrBuilder(builder->subarrayStart());
        for (auto&& value : getArray()) {
            value.addToBsonArray(&subarrBuilder, recursionLevel + 1);
        }
        subarrBuilder.doneFast();
    } else {
        *builder << *this;
    }
}

bool Value::coerceToBool() const {
    // TODO Unify the implementation with BSONElement::trueValue().
    switch (getType()) {
        case CodeWScope:
        case MinKey:
        case DBRef:
        case Code:
        case MaxKey:
        case String:
        case Object:
        case Array:
        case BinData:
        case jstOID:
        case Date:
        case RegEx:
        case Symbol:
        case bsonTimestamp:
            return true;

        case EOO:
        case jstNULL:
        case Undefined:
            return false;

        case Bool:
            return _storage.boolValue;
        case NumberInt:
            return _storage.intValue;
        case NumberLong:
            return _storage.longValue;
        case NumberDouble:
            return _storage.doubleValue;
        case NumberDecimal:
            return !_storage.getDecimal().isZero();
    }
    MONGO_verify(false);
}

namespace {

template <typename T>
void assertValueInRangeInt(const T& val) {
    uassert(31108,
            str::stream() << "Can't coerce out of range value " << val << " to int",
            val >= std::numeric_limits<int32_t>::min() &&
                val <= std::numeric_limits<int32_t>::max());
}

template <typename T>
void assertValueInRangeLong(const T& val) {
    uassert(31109,
            str::stream() << "Can't coerce out of range value " << val << " to long",
            val >= std::numeric_limits<long long>::min() &&
                val < BSONElement::kLongLongMaxPlusOneAsDouble);
}
}  // namespace

int Value::coerceToInt() const {
    switch (getType()) {
        case NumberInt:
            return _storage.intValue;

        case NumberLong:
            assertValueInRangeInt(_storage.longValue);
            return static_cast<int>(_storage.longValue);

        case NumberDouble:
            assertValueInRangeInt(_storage.doubleValue);
            return static_cast<int>(_storage.doubleValue);

        case NumberDecimal:
            assertValueInRangeInt(_storage.getDecimal().toDouble());
            return (_storage.getDecimal()).toInt();

        default:
            uassert(16003,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to int",
                    false);
    }  // switch(getType())
}

long long Value::coerceToLong() const {
    switch (getType()) {
        case NumberLong:
            return _storage.longValue;

        case NumberInt:
            return static_cast<long long>(_storage.intValue);

        case NumberDouble:
            assertValueInRangeLong(_storage.doubleValue);
            return static_cast<long long>(_storage.doubleValue);

        case NumberDecimal:
            assertValueInRangeLong(_storage.doubleValue);
            return (_storage.getDecimal()).toLong();

        default:
            uassert(16004,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to long",
                    false);
    }  // switch(getType())
}

double Value::coerceToDouble() const {
    switch (getType()) {
        case NumberDouble:
            return _storage.doubleValue;

        case NumberInt:
            return static_cast<double>(_storage.intValue);

        case NumberLong:
            return static_cast<double>(_storage.longValue);

        case NumberDecimal:
            return (_storage.getDecimal()).toDouble();

        default:
            uassert(16005,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to double",
                    false);
    }  // switch(getType())
}

Decimal128 Value::coerceToDecimal() const {
    switch (getType()) {
        case NumberDecimal:
            return _storage.getDecimal();

        case NumberInt:
            return Decimal128(static_cast<int32_t>(_storage.intValue));

        case NumberLong:
            return Decimal128(static_cast<int64_t>(_storage.longValue));

        case NumberDouble:
            return Decimal128(_storage.doubleValue);

        default:
            uassert(16008,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to decimal",
                    false);
    }  // switch(getType())
}

Date_t Value::coerceToDate() const {
    switch (getType()) {
        case Date:
            return getDate();

        case bsonTimestamp:
            return Date_t::fromMillisSinceEpoch(getTimestamp().getSecs() * 1000LL);

        case jstOID:
            return getOid().asDateT();

        default:
            uassert(16006,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to Date",
                    false);
    }  // switch(getType())
}

string Value::coerceToString() const {
    switch (getType()) {
        case NumberDouble:
            return str::stream() << _storage.doubleValue;

        case NumberInt:
            return str::stream() << _storage.intValue;

        case NumberLong:
            return str::stream() << _storage.longValue;

        case NumberDecimal:
            return str::stream() << _storage.getDecimal().toString();

        case Code:
        case Symbol:
        case String:
            return getRawData().toString();

        case bsonTimestamp:
            return getTimestamp().toStringPretty();

        case Date:
            return uassertStatusOKWithContext(
                TimeZoneDatabase::utcZone().formatDate(kIsoFormatStringZ, getDate()),
                "failed while coercing date to string");

        case EOO:
        case jstNULL:
        case Undefined:
            return "";

        default:
            uassert(16007,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to String",
                    false);
    }  // switch(getType())
}

Timestamp Value::coerceToTimestamp() const {
    switch (getType()) {
        case bsonTimestamp:
            return getTimestamp();

        default:
            uassert(16378,
                    str::stream() << "can't convert from BSON type " << typeName(getType())
                                  << " to timestamp",
                    false);
    }  // switch(getType())
}

// Helper function for Value::compare.
// Better than l-r for cases where difference > MAX_INT
template <typename T>
inline static int cmp(const T& left, const T& right) {
    if (left < right) {
        return -1;
    } else if (left == right) {
        return 0;
    } else {
        dassert(left > right);
        return 1;
    }
}

int Value::compare(const Value& rL, const Value& rR, const StringDataComparator* stringComparator) {
    // Note, this function needs to behave identically to BSONElement::compareElements().
    // Additionally, any changes here must be replicated in hash_combine().
    BSONType lType = rL.getType();
    BSONType rType = rR.getType();

    int ret = lType == rType ? 0  // fast-path common case
                             : cmp(canonicalizeBSONType(lType), canonicalizeBSONType(rType));

    if (ret)
        return ret;

    switch (lType) {
        // Order of types is the same as in BSONElement::compareElements() to make it easier to
        // verify.

        // These are valueless types
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            return ret;

        case Bool:
            return rL.getBool() - rR.getBool();

        case bsonTimestamp:  // unsigned
            return cmp(rL._storage.timestampValue, rR._storage.timestampValue);

        case Date:  // signed
            return cmp(rL._storage.dateValue, rR._storage.dateValue);

            // Numbers should compare by equivalence even if different types

        case NumberDecimal: {
            switch (rType) {
                case NumberDecimal:
                    return compareDecimals(rL._storage.getDecimal(), rR._storage.getDecimal());
                case NumberInt:
                    return compareDecimalToInt(rL._storage.getDecimal(), rR._storage.intValue);
                case NumberLong:
                    return compareDecimalToLong(rL._storage.getDecimal(), rR._storage.longValue);
                case NumberDouble:
                    return compareDecimalToDouble(rL._storage.getDecimal(),
                                                  rR._storage.doubleValue);
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case NumberInt: {
            // All types can precisely represent all NumberInts, so it is safe to simply convert to
            // whatever rhs's type is.
            switch (rType) {
                case NumberInt:
                    return compareInts(rL._storage.intValue, rR._storage.intValue);
                case NumberLong:
                    return compareLongs(rL._storage.intValue, rR._storage.longValue);
                case NumberDouble:
                    return compareDoubles(rL._storage.intValue, rR._storage.doubleValue);
                case NumberDecimal:
                    return compareIntToDecimal(rL._storage.intValue, rR._storage.getDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case NumberLong: {
            switch (rType) {
                case NumberLong:
                    return compareLongs(rL._storage.longValue, rR._storage.longValue);
                case NumberInt:
                    return compareLongs(rL._storage.longValue, rR._storage.intValue);
                case NumberDouble:
                    return compareLongToDouble(rL._storage.longValue, rR._storage.doubleValue);
                case NumberDecimal:
                    return compareLongToDecimal(rL._storage.longValue, rR._storage.getDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case NumberDouble: {
            switch (rType) {
                case NumberDouble:
                    return compareDoubles(rL._storage.doubleValue, rR._storage.doubleValue);
                case NumberInt:
                    return compareDoubles(rL._storage.doubleValue, rR._storage.intValue);
                case NumberLong:
                    return compareDoubleToLong(rL._storage.doubleValue, rR._storage.longValue);
                case NumberDecimal:
                    return compareDoubleToDecimal(rL._storage.doubleValue,
                                                  rR._storage.getDecimal());
                default:
                    MONGO_UNREACHABLE;
            }
        }

        case jstOID:
            return memcmp(rL._storage.oid, rR._storage.oid, OID::kOIDSize);

        case String: {
            if (!stringComparator) {
                return rL.getStringData().compare(rR.getRawData());
            }

            return stringComparator->compare(rL.getStringData(), rR.getRawData());
        }

        case Code:
        case Symbol:
            return rL.getRawData().compare(rR.getRawData());

        case Object:
            return Document::compare(rL.getDocument(), rR.getDocument(), stringComparator);

        case Array: {
            const vector<Value>& lArr = rL.getArray();
            const vector<Value>& rArr = rR.getArray();

            const size_t elems = std::min(lArr.size(), rArr.size());
            for (size_t i = 0; i < elems; i++) {
                // compare the two corresponding elements
                ret = Value::compare(lArr[i], rArr[i], stringComparator);
                if (ret)
                    return ret;  // values are unequal
            }

            // if we get here we are either equal or one is prefix of the other
            return cmp(lArr.size(), rArr.size());
        }

        case DBRef: {
            intrusive_ptr<const RCDBRef> l = rL._storage.getDBRef();
            intrusive_ptr<const RCDBRef> r = rR._storage.getDBRef();
            ret = cmp(l->ns.size(), r->ns.size());
            if (ret)
                return ret;

            return l->oid.compare(r->oid);
        }

        case BinData: {
            ret = cmp(rL.getRawData().size(), rR.getRawData().size());
            if (ret)
                return ret;

            // Need to compare as an unsigned char rather than enum since BSON uses memcmp
            ret = cmp(rL._storage.binSubType, rR._storage.binSubType);
            if (ret)
                return ret;

            return rL.getRawData().compare(rR.getRawData());
        }

        case RegEx:
            // same as String in this impl but keeping order same as
            // BSONElement::compareElements().
            return rL.getRawData().compare(rR.getRawData());

        case CodeWScope: {
            intrusive_ptr<const RCCodeWScope> l = rL._storage.getCodeWScope();
            intrusive_ptr<const RCCodeWScope> r = rR._storage.getCodeWScope();

            ret = l->code.compare(r->code);
            if (ret)
                return ret;

            return l->scope.woCompare(r->scope);
        }
    }
    MONGO_verify(false);
}

namespace {
/**
 * Hashes the given 'StringData', combines the resulting hash with 'seed', and returns the result.
 */
size_t hashStringData(StringData sd, size_t seed) {
    size_t strHash = absl::Hash<absl::string_view>{}(absl::string_view(sd.data(), sd.size()));
    boost::hash_combine(seed, strHash);
    return seed;
}
}  // namespace

void Value::hash_combine(size_t& seed, const StringDataComparator* stringComparator) const {
    BSONType type = getType();

    boost::hash_combine(seed, canonicalizeBSONType(type));

    switch (type) {
        // Order of types is the same as in Value::compare() and BSONElement::compareElements().

        // These are valueless types
        case EOO:
        case Undefined:
        case jstNULL:
        case MaxKey:
        case MinKey:
            return;

        case Bool:
            boost::hash_combine(seed, getBool());
            break;

        case bsonTimestamp:
        case Date:
            MONGO_STATIC_ASSERT(sizeof(_storage.dateValue) == sizeof(_storage.timestampValue));
            boost::hash_combine(seed, _storage.dateValue);
            break;

        case mongo::NumberDecimal: {
            const Decimal128 dcml = getDecimal();
            if (dcml.toAbs().isGreater(Decimal128(std::numeric_limits<double>::max(),
                                                  Decimal128::kRoundTo34Digits,
                                                  Decimal128::kRoundTowardZero)) &&
                !dcml.isInfinite() && !dcml.isNaN()) {
                // Normalize our decimal to force equivalent decimals
                // in the same cohort to hash to the same value
                Decimal128 dcmlNorm(dcml.normalize());
                boost::hash_combine(seed, dcmlNorm.getValue().low64);
                boost::hash_combine(seed, dcmlNorm.getValue().high64);
                break;
            }
            // Else, fall through and convert the decimal to a double and hash.
            // At this point the decimal fits into the range of doubles, is infinity, or is NaN,
            // which doubles have a cheaper representation for.
            [[fallthrough]];
        }
        // This converts all numbers to doubles, which ignores the low-order bits of
        // NumberLongs > 2**53 and precise decimal numbers without double representations,
        // but that is ok since the hash will still be the same for equal numbers and is
        // still likely to be different for different numbers. (Note: this issue only
        // applies for decimals when they are inside of the valid double range. See
        // the above case.)
        // SERVER-16851
        case NumberDouble:
        case NumberLong:
        case NumberInt: {
            const double dbl = getDouble();
            if (std::isnan(dbl)) {
                boost::hash_combine(seed, numeric_limits<double>::quiet_NaN());
            } else {
                boost::hash_combine(seed, dbl);
            }
            break;
        }

        case jstOID:
            getOid().hash_combine(seed);
            break;

        case Code:
        case Symbol: {
            StringData sd = getRawData();
            seed = hashStringData(sd, seed);
            break;
        }

        case String: {
            StringData sd = getStringData();
            if (stringComparator) {
                stringComparator->hash_combine(seed, sd);
            } else {
                seed = hashStringData(sd, seed);
            }
            break;
        }

        case Object:
            getDocument().hash_combine(seed, stringComparator);
            break;

        case Array: {
            const vector<Value>& vec = getArray();
            for (size_t i = 0; i < vec.size(); i++)
                vec[i].hash_combine(seed, stringComparator);
            break;
        }

        case DBRef:
            boost::hash_combine(seed, _storage.getDBRef()->ns);
            _storage.getDBRef()->oid.hash_combine(seed);
            break;


        case BinData: {
            StringData sd = getRawData();
            seed = hashStringData(sd, seed);
            boost::hash_combine(seed, _storage.binDataType());
            break;
        }

        case RegEx: {
            StringData sd = getRawData();
            seed = hashStringData(sd, seed);
            break;
        }

        case CodeWScope: {
            intrusive_ptr<const RCCodeWScope> cws = _storage.getCodeWScope();
            simpleStringDataComparator.hash_combine(seed, cws->code);
            SimpleBSONObjComparator::kInstance.hash_combine(seed, cws->scope);
            break;
        }
    }
}

BSONType Value::getWidestNumeric(BSONType lType, BSONType rType) {
    if (lType == NumberDouble) {
        switch (rType) {
            case NumberDecimal:
                return NumberDecimal;

            case NumberDouble:
            case NumberLong:
            case NumberInt:
                return NumberDouble;

            default:
                break;
        }
    } else if (lType == NumberLong) {
        switch (rType) {
            case NumberDecimal:
                return NumberDecimal;

            case NumberDouble:
                return NumberDouble;

            case NumberLong:
            case NumberInt:
                return NumberLong;

            default:
                break;
        }
    } else if (lType == NumberInt) {
        switch (rType) {
            case NumberDecimal:
                return NumberDecimal;

            case NumberDouble:
                return NumberDouble;

            case NumberLong:
                return NumberLong;

            case NumberInt:
                return NumberInt;

            default:
                break;
        }
    } else if (lType == NumberDecimal) {
        switch (rType) {
            case NumberInt:
            case NumberLong:
            case NumberDouble:
            case NumberDecimal:
                return NumberDecimal;

            default:
                break;
        }
    }

    // Reachable, but callers must subsequently err out in this case.
    return Undefined;
}

bool Value::integral() const {
    switch (getType()) {
        case NumberInt:
            return true;
        case NumberLong:
            return bool(representAs<int>(_storage.longValue));
        case NumberDouble:
            return bool(representAs<int>(_storage.doubleValue));
        case NumberDecimal: {
            // If we are able to convert the decimal to an int32_t without any rounding errors,
            // then it is integral.
            uint32_t signalingFlags = Decimal128::kNoFlag;
            (void)_storage.getDecimal().toIntExact(&signalingFlags);
            return signalingFlags == Decimal128::kNoFlag;
        }
        default:
            return false;
    }
}

bool Value::isNaN() const {
    switch (getType()) {
        case NumberInt:
        case NumberLong:
        case NumberDouble: {
            const double dbl = getDouble();
            return std::isnan(dbl);
        }
        case NumberDecimal: {
            return _storage.getDecimal().isNaN();
        }

        default:
            return false;
    }
}

bool Value::isInfinite() const {
    switch (getType()) {
        case NumberDouble:
            return (_storage.doubleValue == std::numeric_limits<double>::infinity() ||
                    _storage.doubleValue == -std::numeric_limits<double>::infinity());
        case NumberDecimal:
            return _storage.getDecimal().isInfinite();

        default:
            return false;
    }
}

bool Value::integral64Bit() const {
    switch (getType()) {
        case NumberInt:
        case NumberLong:
            return true;
        case NumberDouble:
            return bool(representAs<int64_t>(_storage.doubleValue));
        case NumberDecimal: {
            // If we are able to convert the decimal to an int64_t without any rounding errors,
            // then it is a 64-bit.
            uint32_t signalingFlags = Decimal128::kNoFlag;
            (void)_storage.getDecimal().toLongExact(&signalingFlags);
            return signalingFlags == Decimal128::kNoFlag;
        }
        default:
            return false;
    }
}

size_t Value::getApproximateSize() const {
    switch (getType()) {
        case Code:
        case RegEx:
        case Symbol:
        case BinData:
        case String:
            return sizeof(Value) +
                (_storage.shortStr ? 0  // string stored inline, so no extra mem usage
                                   : sizeof(RCString) + _storage.getString().size());

        case Object:
            return sizeof(Value) + getDocument().getApproximateSize();

        case Array: {
            size_t size = sizeof(Value);
            size += sizeof(RCVector<Value>);
            const size_t n = getArray().size();
            for (size_t i = 0; i < n; ++i) {
                size += getArray()[i].getApproximateSize();
            }
            return size;
        }

        case CodeWScope:
            return sizeof(Value) + sizeof(RCCodeWScope) + _storage.getCodeWScope()->code.size() +
                _storage.getCodeWScope()->scope.objsize();

        case DBRef:
            return sizeof(Value) + sizeof(RCDBRef) + _storage.getDBRef()->ns.size();

        case NumberDecimal:
            return sizeof(Value) + sizeof(RCDecimal);

        // These types are always contained within the Value
        case EOO:
        case MinKey:
        case MaxKey:
        case NumberDouble:
        case jstOID:
        case Bool:
        case Date:
        case NumberInt:
        case bsonTimestamp:
        case NumberLong:
        case jstNULL:
        case Undefined:
            return sizeof(Value);
    }
    MONGO_verify(false);
}

string Value::toString() const {
    // TODO use StringBuilder when operator << is ready
    stringstream out;
    out << *this;
    return out.str();
}

ostream& operator<<(ostream& out, const Value& val) {
    switch (val.getType()) {
        case EOO:
            return out << "MISSING";
        case MinKey:
            return out << "MinKey";
        case MaxKey:
            return out << "MaxKey";
        case jstOID:
            return out << val.getOid();
        case String:
            return out << '"' << val.getString() << '"';
        case RegEx:
            return out << '/' << val.getRegex() << '/' << val.getRegexFlags();
        case Symbol:
            return out << "Symbol(\"" << val.getSymbol() << "\")";
        case Code:
            return out << "Code(\"" << val.getCode() << "\")";
        case Bool:
            return out << (val.getBool() ? "true" : "false");
        case NumberDecimal:
            return out << val.getDecimal().toString();
        case NumberDouble:
            return out << val.getDouble();
        case NumberLong:
            return out << val.getLong();
        case NumberInt:
            return out << val.getInt();
        case jstNULL:
            return out << "null";
        case Undefined:
            return out << "undefined";
        case Date:
            return out << [&] {
                if (auto string = TimeZoneDatabase::utcZone().formatDate(kIsoFormatStringZ,
                                                                         val.coerceToDate());
                    string.isOK())
                    return string.getValue();
                else
                    return "illegal date"s;
            }();
        case bsonTimestamp:
            return out << val.getTimestamp().toString();
        case Object:
            return out << val.getDocument().toString();
        case Array: {
            out << "[";
            const size_t n = val.getArray().size();
            for (size_t i = 0; i < n; i++) {
                if (i)
                    out << ", ";
                out << val.getArray()[i];
            }
            out << "]";
            return out;
        }

        case CodeWScope:
            return out << "CodeWScope(\"" << val._storage.getCodeWScope()->code << "\", "
                       << val._storage.getCodeWScope()->scope << ')';

        case BinData:
            return out << "BinData(" << val._storage.binDataType() << ", \""
                       << hexblob::encode(val._storage.getString()) << "\")";

        case DBRef:
            return out << "DBRef(\"" << val._storage.getDBRef()->ns << "\", "
                       << val._storage.getDBRef()->oid << ')';
    }

    // Not in default case to trigger better warning if a case is missing
    MONGO_verify(false);
}

Value Value::shred() const {
    if (isObject()) {
        return Value(getDocument().shred());
    } else if (isArray()) {
        std::vector<Value> values;
        for (auto&& val : getArray()) {
            values.push_back(val.shred());
        }
        return Value(values);
    }
    return Value(*this);
}

void Value::serializeForSorter(BufBuilder& buf) const {
    buf.appendChar(getType());
    switch (getType()) {
        // type-only types
        case EOO:
        case MinKey:
        case MaxKey:
        case jstNULL:
        case Undefined:
            break;

        // simple types
        case jstOID:
            buf.appendStruct(_storage.oid);
            break;
        case NumberInt:
            buf.appendNum(_storage.intValue);
            break;
        case NumberLong:
            buf.appendNum(_storage.longValue);
            break;
        case NumberDouble:
            buf.appendNum(_storage.doubleValue);
            break;
        case NumberDecimal:
            buf.appendNum(_storage.getDecimal());
            break;
        case Bool:
            buf.appendChar(_storage.boolValue);
            break;
        case Date:
            buf.appendNum(_storage.dateValue);
            break;
        case bsonTimestamp:
            buf.appendStruct(getTimestamp());
            break;

        // types that are like strings
        case String:
        case Symbol:
        case Code: {
            StringData str = getRawData();
            buf.appendNum(int(str.size()));
            buf.appendStrBytes(str);
            break;
        }

        case BinData: {
            StringData str = getRawData();
            buf.appendChar(_storage.binDataType());
            buf.appendNum(int(str.size()));
            buf.appendStrBytes(str);
            break;
        }

        case RegEx:
            buf.appendCStr(getRegex());
            buf.appendCStr(getRegexFlags());
            break;

        case Object:
            getDocument().serializeForSorter(buf);
            break;

        case DBRef:
            buf.appendStruct(_storage.getDBRef()->oid);
            buf.appendCStr(_storage.getDBRef()->ns);
            break;

        case CodeWScope: {
            intrusive_ptr<const RCCodeWScope> cws = _storage.getCodeWScope();
            buf.appendNum(int(cws->code.size()));
            buf.appendStrBytes(cws->code);
            cws->scope.serializeForSorter(buf);
            break;
        }

        case Array: {
            const vector<Value>& array = getArray();
            const int numElems = array.size();
            buf.appendNum(numElems);
            for (int i = 0; i < numElems; i++)
                array[i].serializeForSorter(buf);
            break;
        }
    }
}

Value Value::deserializeForSorter(BufReader& buf, const SorterDeserializeSettings& settings) {
    const BSONType type = BSONType(buf.read<signed char>());  // need sign extension for MinKey
    switch (type) {
        // type-only types
        case EOO:
        case MinKey:
        case MaxKey:
        case jstNULL:
        case Undefined:
            return Value(ValueStorage(type));

        // simple types
        case jstOID:
            return Value(OID::from(buf.skip(OID::kOIDSize)));
        case NumberInt:
            return Value(buf.read<LittleEndian<int>>().value);
        case NumberLong:
            return Value(buf.read<LittleEndian<long long>>().value);
        case NumberDouble:
            return Value(buf.read<LittleEndian<double>>().value);
        case NumberDecimal: {
            auto lo = buf.read<LittleEndian<std::uint64_t>>().value;
            auto hi = buf.read<LittleEndian<std::uint64_t>>().value;
            return Value(Decimal128{Decimal128::Value{lo, hi}});
        }
        case Bool:
            return Value(bool(buf.read<char>()));
        case Date:
            return Value(Date_t::fromMillisSinceEpoch(buf.read<LittleEndian<long long>>().value));
        case bsonTimestamp:
            return Value(buf.read<Timestamp>());

        // types that are like strings
        case String:
        case Symbol:
        case Code: {
            int size = buf.read<LittleEndian<int>>();
            const char* str = static_cast<const char*>(buf.skip(size));
            return Value(ValueStorage(type, StringData(str, size)));
        }

        case BinData: {
            BinDataType bdt = BinDataType(buf.read<unsigned char>());
            int size = buf.read<LittleEndian<int>>();
            const void* data = buf.skip(size);
            return Value(BSONBinData(data, size, bdt));
        }

        case RegEx: {
            StringData regex = buf.readCStr();
            StringData flags = buf.readCStr();
            return Value(BSONRegEx(regex, flags));
        }

        case Object:
            return Value(
                Document::deserializeForSorter(buf, Document::SorterDeserializeSettings()));

        case DBRef: {
            OID oid = OID::from(buf.skip(OID::kOIDSize));
            StringData ns = buf.readCStr();
            return Value(BSONDBRef(ns, oid));
        }

        case CodeWScope: {
            int size = buf.read<LittleEndian<int>>();
            const char* str = static_cast<const char*>(buf.skip(size));
            BSONObj bson = BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings());
            return Value(BSONCodeWScope(StringData(str, size), bson));
        }

        case Array: {
            const int numElems = buf.read<LittleEndian<int>>();
            vector<Value> array;
            array.reserve(numElems);
            for (int i = 0; i < numElems; i++)
                array.push_back(deserializeForSorter(buf, settings));
            return Value(std::move(array));
        }
    }
    MONGO_verify(false);
}

void Value::serializeForIDL(StringData fieldName, BSONObjBuilder* builder) const {
    addToBsonObj(builder, fieldName);
}

void Value::serializeForIDL(BSONArrayBuilder* builder) const {
    addToBsonArray(builder);
}

Value Value::deserializeForIDL(const BSONElement& element) {
    return Value(element);
}

BSONObj Value::wrap(StringData newName) const {
    BSONObjBuilder b(getApproximateSize() + 6 + newName.size());
    addToBsonObj(&b, newName);
    return b.obj();
}

}  // namespace mongo
