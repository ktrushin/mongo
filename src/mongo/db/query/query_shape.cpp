/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape.h"

#include "mongo/base/status.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo::query_shape {

BSONObj debugPredicateShape(const MatchExpression* predicate) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    return predicate->serialize(opts);
}
BSONObj representativePredicateShape(const MatchExpression* predicate) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    return predicate->serialize(opts);
}

BSONObj debugPredicateShape(const MatchExpression* predicate,
                            std::function<std::string(StringData)> transformIdentifiersCallback) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;
    opts.transformIdentifiersCallback = transformIdentifiersCallback;
    opts.transformIdentifiers = true;
    return predicate->serialize(opts);
}

BSONObj representativePredicateShape(
    const MatchExpression* predicate,
    std::function<std::string(StringData)> transformIdentifiersCallback) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;
    opts.transformIdentifiersCallback = transformIdentifiersCallback;
    opts.transformIdentifiers = true;
    return predicate->serialize(opts);
}

BSONObj extractSortShape(const BSONObj& sortSpec,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const SerializationOptions& opts) {
    if (sortSpec.isEmpty()) {
        return sortSpec;
    }
    auto natural = sortSpec[query_request_helper::kNaturalSortField];

    if (!natural) {
        return SortPattern{sortSpec, expCtx}
            .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
            .toBson();
    }
    // This '$natural' will fail to parse as a valid SortPattern since it is not a valid field
    // path - it is usually considered and converted into a hint. For the query shape, we'll
    // keep it unmodified.
    BSONObjBuilder bob;
    for (auto&& elem : sortSpec) {
        if (elem.isABSONObj()) {
            // We expect this won't work or parse on the main command path, but for shapification we
            // don't really care, just treat it as a literal and don't bother parsing.
            bob << opts.serializeFieldPathFromString(elem.fieldNameStringData())
                << kLiteralArgString;
        } else if (elem.fieldNameStringData() == natural.fieldNameStringData()) {
            bob.append(elem);
        } else {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldNameStringData()));
        }
    }
    return bob.obj();
}

static std::string hintSpecialField = "$hint";
void addShapeLiterals(BSONObjBuilder* bob,
                      const FindCommandRequest& findCommand,
                      const SerializationOptions& opts) {
    if (auto limit = findCommand.getLimit()) {
        opts.appendLiteral(
            bob, FindCommandRequest::kLimitFieldName, static_cast<long long>(*limit));
    }
    if (auto skip = findCommand.getSkip()) {
        opts.appendLiteral(bob, FindCommandRequest::kSkipFieldName, static_cast<long long>(*skip));
    }
}

static std::vector<
    std::pair<StringData, std::function<const OptionalBool(const FindCommandRequest&)>>>
    boolArgMap = {
        {FindCommandRequest::kSingleBatchFieldName, &FindCommandRequest::getSingleBatch},
        {FindCommandRequest::kAllowDiskUseFieldName, &FindCommandRequest::getAllowDiskUse},
        {FindCommandRequest::kReturnKeyFieldName, &FindCommandRequest::getReturnKey},
        {FindCommandRequest::kShowRecordIdFieldName, &FindCommandRequest::getShowRecordId},
        {FindCommandRequest::kTailableFieldName, &FindCommandRequest::getTailable},
        {FindCommandRequest::kAwaitDataFieldName, &FindCommandRequest::getAwaitData},
        {FindCommandRequest::kMirroredFieldName, &FindCommandRequest::getMirrored},
        {FindCommandRequest::kOplogReplayFieldName, &FindCommandRequest::getOplogReplay},
};
std::vector<std::pair<StringData, std::function<const BSONObj(const FindCommandRequest&)>>>
    objArgMap = {
        {FindCommandRequest::kCollationFieldName, &FindCommandRequest::getCollation},

};

void addRemainingFindCommandFields(BSONObjBuilder* bob,
                                   const FindCommandRequest& findCommand,
                                   const SerializationOptions& opts) {
    for (auto [fieldName, getterFunction] : boolArgMap) {
        auto optBool = getterFunction(findCommand);
        if (optBool.has_value()) {
            opts.appendLiteral(bob, fieldName, optBool.value_or(false));
        }
    }
    auto collation = findCommand.getCollation();
    if (!collation.isEmpty()) {
        opts.appendLiteral(bob, FindCommandRequest::kCollationFieldName, collation);
    }
}

BSONObj extractHintShape(BSONObj obj, const SerializationOptions& opts, bool redactValues) {
    BSONObjBuilder bob;
    for (BSONElement elem : obj) {
        if (hintSpecialField.compare(elem.fieldName()) == 0) {
            if (elem.type() == BSONType::String) {
                bob.append(hintSpecialField, opts.serializeFieldPathFromString(elem.String()));
            } else if (elem.type() == BSONType::Object) {
                opts.appendLiteral(&bob, hintSpecialField, elem.Obj());
            } else {
                uasserted(ErrorCodes::FailedToParse, "$hint must be a string or an object");
            }
            continue;
        }

        // $natural doesn't need to be redacted.
        if (elem.fieldNameStringData().compare(query_request_helper::kNaturalSortField) == 0) {
            bob.append(elem);
            continue;
        }

        if (opts.replacementForLiteralArgs && redactValues) {
            bob.append(opts.serializeFieldPathFromString(elem.fieldName()),
                       opts.replacementForLiteralArgs.get());
        } else {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldName()));
        }
    }
    return bob.obj();
}

/**
 * In a let specification all field names are variable names, and all values are either expressions
 * or constants.
 */
BSONObj extractLetSpecShape(BSONObj letSpec,
                            const SerializationOptions& opts,
                            boost::intrusive_ptr<ExpressionContext> expCtx) {

    BSONObjBuilder bob;
    for (BSONElement elem : letSpec) {
        auto redactedValue =
            Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState)
                ->serialize(opts);
        // Note that this will throw on deeply nested let variables.
        redactedValue.addToBsonObj(&bob, opts.serializeFieldPathFromString(elem.fieldName()));
    }
    return bob.obj();
}

BSONObj extractNamespaceShape(NamespaceString nss, const SerializationOptions& opts) {
    BSONObjBuilder bob;
    if (nss.tenantId()) {
        bob.append("tenantId", opts.serializeIdentifier(nss.tenantId().value().toString()));
    }
    bob.append("db", opts.serializeIdentifier(nss.db()));
    bob.append("coll", opts.serializeIdentifier(nss.coll()));
    return bob.obj();
}

BSONObj extractQueryShape(const ParsedFindCommand& findRequest,
                          const SerializationOptions& opts,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& findCmd = *findRequest.findCommandRequest;
    BSONObjBuilder bob;
    // Serialize the namespace as part of the query shape.
    {
        auto ns = findCmd.getNamespaceOrUUID();
        if (ns.isNamespaceString()) {
            bob.append("cmdNs", extractNamespaceShape(ns.nss(), opts));
        } else {
            BSONObjBuilder cmdNs = bob.subobjStart("cmdNs");
            cmdNs.append("uuid", opts.serializeIdentifier(ns.uuid().toString()));
            cmdNs.done();
        }
    }

    bob.append("command", "find");
    std::unique_ptr<MatchExpression> filterExpr;
    // Filter.
    bob.append(FindCommandRequest::kFilterFieldName, findRequest.filter->serialize(opts));

    // Let Spec.
    if (auto letSpec = findCmd.getLet()) {
        auto redactedObj = extractLetSpecShape(letSpec.get(), opts, expCtx);
        auto ownedObj = redactedObj.getOwned();
        bob.append(FindCommandRequest::kLetFieldName, std::move(ownedObj));
    }

    if (findRequest.proj) {
        bob.append(FindCommandRequest::kProjectionFieldName,
                   projection_ast::serialize(*findRequest.proj->root(), opts));
    }

    // Assume the hint is correct and contains field names. It is possible that this hint
    // doesn't actually represent an index, but we can't detect that here.
    // Hint, max, and min won't serialize if the object is empty.
    if (!findCmd.getHint().isEmpty()) {
        bob.append(FindCommandRequest::kHintFieldName,
                   extractHintShape(findCmd.getHint(), opts, false));
        // Max/Min aren't valid without hint.
        if (!findCmd.getMax().isEmpty()) {
            bob.append(FindCommandRequest::kMaxFieldName,
                       extractHintShape(findCmd.getMax(), opts, true));
        }
        if (!findCmd.getMin().isEmpty()) {
            bob.append(FindCommandRequest::kMinFieldName,
                       extractHintShape(findCmd.getMin(), opts, true));
        }
    }

    // Sort.
    if (findRequest.sort) {
        bob.append(
            FindCommandRequest::kSortFieldName,
            findRequest.sort
                ->serialize(SortPattern::SortKeySerialization::kForPipelineSerialization, opts)
                .toBson());
    }

    // Fields for literal redaction. Adds limit and skip.
    addShapeLiterals(&bob, findCmd, opts);

    // Add the fields that require no redaction.
    addRemainingFindCommandFields(&bob, findCmd, opts);

    return bob.obj();
}

BSONObj extractQueryShape(const AggregateCommandRequest& aggregateCommand,
                          const Pipeline& pipeline,
                          const SerializationOptions& opts,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    BSONObjBuilder bob;

    // namespace
    bob.append("cmdNs", extractNamespaceShape(aggregateCommand.getNamespace(), opts));
    bob.append("command", "aggregate");

    // pipeline
    {
        BSONArrayBuilder pipelineBab(
            bob.subarrayStart(AggregateCommandRequest::kPipelineFieldName));
        auto serializedPipeline = pipeline.serializeToBson(opts);
        for (const auto& stage : serializedPipeline) {
            pipelineBab.append(stage);
        }
        pipelineBab.doneFast();
    }

    // explain
    if (aggregateCommand.getExplain().has_value()) {
        bob.append(AggregateCommandRequest::kExplainFieldName, true);
    }

    // allowDiskUse
    if (auto param = aggregateCommand.getAllowDiskUse(); param.has_value()) {
        bob.append(AggregateCommandRequest::kAllowDiskUseFieldName, param.value_or(false));
    }

    // collation
    if (auto param = aggregateCommand.getCollation()) {
        bob.append(AggregateCommandRequest::kCollationFieldName, param.get());
    }

    // hint
    if (auto hint = aggregateCommand.getHint()) {
        bob.append(AggregateCommandRequest::kHintFieldName,
                   extractHintShape(hint.get(), opts, false));
    }

    // let
    if (auto letSpec = aggregateCommand.getLet()) {
        auto redactedObj = extractLetSpecShape(letSpec.get(), opts, expCtx);
        auto ownedObj = redactedObj.getOwned();
        bob.append(FindCommandRequest::kLetFieldName, std::move(ownedObj));
    }
    return bob.obj();
}
}  // namespace mongo::query_shape