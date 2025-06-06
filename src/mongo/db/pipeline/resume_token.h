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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

struct ResumeTokenData {
    /*
     * The default or "preferred" token version generated by this version of the server.
     */
    static constexpr int kDefaultTokenVersion = 2;

    /**
     * Flag to indicate if the resume token is from an invalidate notification.
     */
    enum FromInvalidate : bool {
        kFromInvalidate = true,
        kNotFromInvalidate = false,
    };

    /**
     * Flag to indicate the type of resume token we are generating.
     */
    enum TokenType : int {
        kHighWaterMarkToken = 0,  // Token refers to a point in time, not an event.
        kEventToken = 128,        // Token refers to an actual event in the stream.
    };

    ResumeTokenData(Timestamp clusterTimeIn,
                    int versionIn,
                    size_t txnOpIndexIn,
                    const boost::optional<UUID>& uuidIn,
                    StringData opType,
                    Value documentKey,
                    Value opDescription);

    // This constructor should only be directly called by unit tests.
    ResumeTokenData(Timestamp clusterTimeIn,
                    int versionIn,
                    size_t txnOpIndexIn,
                    const boost::optional<UUID>& uuidIn,
                    Value eventIdentifierIn,
                    FromInvalidate fromInvalidate = FromInvalidate::kNotFromInvalidate,
                    TokenType tokenType = TokenType::kEventToken)
        : clusterTime(clusterTimeIn),
          version(versionIn),
          tokenType(tokenType),
          txnOpIndex(txnOpIndexIn),
          fromInvalidate(fromInvalidate),
          uuid(uuidIn),
          eventIdentifier(std::move(eventIdentifierIn)) {};

    bool operator==(const ResumeTokenData& other) const;
    bool operator!=(const ResumeTokenData& other) const {
        return !(*this == other);
    }

    Timestamp clusterTime;
    int version = kDefaultTokenVersion;
    TokenType tokenType = TokenType::kEventToken;
    // When a resume token references an operation in a transaction, the 'clusterTime' stores the
    // commit time of the transaction, and the 'txnOpIndex' field  stores the index of the operation
    // within its transaction. Operations that are not in a transaction always have a value of 0 for
    // this field.
    size_t txnOpIndex = 0;
    // Flag to indicate that this resume token is from an "invalidate" entry. This will not be set
    // on a token from a command that *would* invalidate a change stream, but rather the invalidate
    // notification itself.
    FromInvalidate fromInvalidate = FromInvalidate::kNotFromInvalidate;
    boost::optional<UUID> uuid;

    // The eventIdentifier can be either be a document key for CRUD operations, or a more
    // descriptive operation details for non-CRUD operations.
    Value eventIdentifier;

    // Index of the current fragment, for oversized events that have been split.
    boost::optional<size_t> fragmentNum;

    BSONObj toBSON() const;

private:
    // This private constructor should only ever be used internally or by the ResumeToken class.
    ResumeTokenData() = default;

    friend class ResumeToken;
};

std::ostream& operator<<(std::ostream& out, const ResumeTokenData& tokenData);

/**
 * A token passed in by the user to indicate where in the oplog we should start for $changeStream.
 * This token has the following format:
 *   {
 *     _data: String, A hex encoding of the binary generated by keystring encoding the clusterTime,
 *            version, txnOpIndex, UUID, then documentKey in that order.
 *     _typeBits: BinData - The keystring type bits used for deserialization.
 *   }
 *   The _data field data is encoded such that string comparisons provide the correct ordering of
 *   tokens. Unlike the BinData, this can be sorted correctly using a MongoDB sort. BinData
 *   unfortunately orders by the length of the data first, then by the contents.
 *
 *   As an optimization, the _typeBits field may be missing and should not affect token comparison.
 */
class ResumeToken {
public:
    constexpr static StringData kDataFieldName = "_data"_sd;
    constexpr static StringData kTypeBitsFieldName = "_typeBits"_sd;

    /**
     * Parse a resume token from a BSON object; used as an interface to the IDL parser.
     */
    static ResumeToken parse(const BSONObj& resumeBson) {
        return ResumeToken::parse(Document(resumeBson));
    }

    static ResumeToken parse(const Document& document);

    /**
     * Generate a high-water-mark token for 'clusterTime', with no UUID or documentKey.
     */
    static ResumeToken makeHighWaterMarkToken(Timestamp clusterTime, int version);

    /**
     * Returns true if the given token data represents a valid high-water-mark resume token; that
     * is, it does not refer to a specific operation, but instead specifies a clusterTime after
     * which the stream should resume.
     */
    static bool isHighWaterMarkToken(const ResumeTokenData& tokenData);

    /**
     * The default no-argument constructor is required by the IDL for types used as non-optional
     * fields.
     */
    ResumeToken() = default;

    /**
     * Parses 'resumeValue' into a ResumeToken using the hex-encoded string format.
     */
    explicit ResumeToken(const ResumeTokenData& resumeValue);

    /**
     * Convenience method to represent the ResumeToken as a Document.
     * Provides support for specifying SerializationOptions, as this method is used to service the
     * toBSON().
     */
    Document toDocument(const SerializationOptions& options = {}) const;

    /**
     * Serialization to BSONObj. Provides support for specifying SerializationOptions,
     * as ResumeToken requires a "query_shape: custom" specification in its IDL uses.
     */
    BSONObj toBSON(const SerializationOptions& options = {}) const;

    ResumeTokenData getData() const;

    Timestamp getClusterTime() const {
        return getData().clusterTime;
    }

    bool operator==(const ResumeToken&) const;
    bool operator!=(const ResumeToken& other) const {
        return !(*this == other);
    }

    friend std::ostream& operator<<(std::ostream& out, const ResumeToken& token) {
        return out << token.getData();
    }

private:
    // Helper function for makeHighWaterMarkToken and isHighWaterMarkToken.
    static ResumeTokenData makeHighWaterMarkTokenData(Timestamp clusterTime, int version);

    explicit ResumeToken(const Document& resumeData);

    // This is the hex-encoded string encoding all the pieces of the resume token.
    std::string _hexKeyString;

    // Since we are using a KeyString encoding, we might lose some information about what the
    // original types of the serialized values were. For example, the integer 2 and the double 2.0
    // will generate the same KeyString. We keep the type bits around so we can deserialize without
    // losing information.
    Value _typeBits;
};
}  // namespace mongo
