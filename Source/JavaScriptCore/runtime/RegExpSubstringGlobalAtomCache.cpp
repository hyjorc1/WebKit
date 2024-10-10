/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RegExpSubstringGlobalAtomCache.h"

#include "JSGlobalObjectInlines.h"
#include "runtime/JSGlobalObject.h"
#include "runtime/JSString.h"
#include "runtime/PropertyName.h"

namespace JSC {

template<typename Visitor>
void RegExpSubstringGlobalAtomCache::visitAggregateImpl(Visitor& visitor)
{
    visitor.append(m_lastSubstringBase);
    visitor.append(m_lastRegExp);
}

DEFINE_VISIT_AGGREGATE(RegExpSubstringGlobalAtomCache);

bool RegExpSubstringGlobalAtomCache::hasValidPattern(JSString* string, RegExp* regExp)
{
    return string->isSubstring() && regExp->global() && regExp->hasValidAtom();
}

void RegExpSubstringGlobalAtomCache::tryGet(JSString* string, RegExp* regExp, RegExpSubstringGlobalAtomCache::Meta& meta)
{
    if (!string->isSubstring())
        return;
    JSRopeString* substring = string->asRope();
    meta.base = substring->substringBase();
    meta.offset = substring->substringOffset();
    meta.length = substring->length();
            
    if (regExp != m_lastRegExp.get())
        return;
    if (substring->substringBase() != m_lastSubstringBase.get())
        return;
    if (substring->substringOffset() != m_lastSubstringOffset)
        return;
    if (substring->length() < m_lastSubstringLength)
        return;

    meta.numberOfMatches = m_lastNumberOfMatches;
    meta.startIndex = m_lastMatchEnd;
}

void RegExpSubstringGlobalAtomCache::tryRecord(VM& vm, JSGlobalObject* globalObject, RegExp* regExp, RegExpSubstringGlobalAtomCache::Meta& meta)
{
    if (!meta.isSubstring())
        return;

    m_lastSubstringBase.setWithoutWriteBarrier(meta.base);
    m_lastSubstringOffset = meta.offset;
    m_lastSubstringLength = meta.length;

    m_lastRegExp.setWithoutWriteBarrier(regExp);
    m_lastNumberOfMatches = meta.numberOfMatches;
    m_lastMatchEnd = meta.startIndex;

    vm.writeBarrier(globalObject);
}

JSValue RegExpSubstringGlobalAtomCache::collectMatches(JSGlobalObject* globalObject, JSString* string, RegExp* regExp)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Try to get the last cache.
    RegExpSubstringGlobalAtomCache::Meta meta;
    tryGet(string, regExp, meta);

    // Keep the substring info above since the following will resolve the substring to a non-rope.
    auto input = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    MatchResult result = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, input, meta.startIndex);
    RETURN_IF_EXCEPTION(scope, { });

    if (result) {
        do {
            if (meta.numberOfMatches > MAX_STORAGE_VECTOR_LENGTH) {
                throwOutOfMemoryError(globalObject, scope);
                return jsUndefined();
            }

            meta.numberOfMatches++;
            meta.startIndex = result.end;
            if (result.empty())
                meta.startIndex++;

            result = globalObject->regExpGlobalData().performMatch(globalObject, regExp, string, input, meta.startIndex);
            RETURN_IF_EXCEPTION(scope, { });
        } while (result);
    } else if (!meta.numberOfMatches)
        return jsNull();

    // Construct the array
    JSArray* array = constructEmptyArray(globalObject, nullptr, meta.numberOfMatches);
    RETURN_IF_EXCEPTION(scope, { });

    const String& atom = regExp->atom();
    ASSERT(!atom.isEmpty() && !atom.isNull());
    JSString* atomString = atom.length() == 1 ? jsSingleCharacterString(vm, atom[0]) : jsNontrivialString(vm, atom);
    for (size_t i = 0, arrayIndex = 0; i < meta.numberOfMatches; ++i) {
        array->putDirectIndex(globalObject, arrayIndex++, atomString);
        RETURN_IF_EXCEPTION(scope, { });
    }

    // Cache
    tryRecord(vm, globalObject, regExp, meta);
    return array;
}

} // namespace JSC
