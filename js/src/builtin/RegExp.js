/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES6 draft rev34 (2015/02/20) 21.2.5.3 get RegExp.prototype.flags
function RegExpFlagsGetter() {
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Step 3.
    var result = "";

    // Steps 4-6.
    if (R.global)
        result += "g";

    // Steps 7-9.
    if (R.ignoreCase)
        result += "i";

    // Steps 10-12.
    if (R.multiline)
        result += "m";

    // Steps 13-15.
    if (R.unicode)
         result += "u";

    // Steps 16-18.
    if (R.sticky)
        result += "y";

    // Step 19.
    return result;
}
_SetCanonicalName(RegExpFlagsGetter, "get flags");

// ES6 draft rc1 21.2.5.14.
function RegExpToString()
{
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Steps 3-4.
    var pattern = R.source;

    // Steps 5-6.
    var flags = R.flags;

    // Step 7.
    return '/' + pattern + '/' + flags;
}
_SetCanonicalName(RegExpToString, "toString");

// ES 2016 draft Mar 25, 2016 21.2.5.2.3.
function AdvanceStringIndex(S, index) {
    // Step 1.
    assert(typeof S === "string", "Expected string as 1st argument");

    // Step 2.
    assert(index >= 0 && index <= MAX_NUMERIC_INDEX, "Expected integer as 2nd argument");

    // Step 3 (skipped).

    // Step 4 (skipped).

    // Step 5.
    var length = S.length;

    // Step 6.
    if (index + 1 >= length)
        return index + 1;

    // Step 7.
    var first = callFunction(std_String_charCodeAt, S, index);

    // Step 8.
    if (first < 0xD800 || first > 0xDBFF)
        return index + 1;

    // Step 9.
    var second = callFunction(std_String_charCodeAt, S, index + 1);

    // Step 10.
    if (second < 0xDC00 || second > 0xDFFF)
        return index + 1;

    // Step 11.
    return index + 2;
}

// ES 2016 draft Mar 25, 2016 21.2.5.6.
function RegExpMatch(string) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Steps 4-5.
    if (!rx.global)
        return RegExpExec(rx, S, false);

    // Step 6.a.
    var fullUnicode = !!rx.unicode;

    // Step 6.b.
    rx.lastIndex = 0;

    // Step 6.c.
    var A = [];

    // Step 6.d.
    var n = 0;

    // Step 6.e.
    while (true) {
        // Step 6.e.i.
        var result = RegExpExec(rx, S, false);

        // Step 6.e.ii.
        if (result === null)
          return (n === 0) ? null : A;

        // Step 6.e.iii.1.
        var matchStr = ToString(result[0]);

        // Step 6.e.iii.2.
        _DefineDataProperty(A, n, matchStr);

        // Step 6.e.iii.4.
        if (matchStr === "") {
            var lastIndex = ToLength(rx.lastIndex);
            rx.lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
        }

        // Step 6.e.iii.5.
        n++;
    }
}

// Checks if following properties and getters are not modified, and accessing
// them not observed by content script:
//   * global
//   * sticky
//   * exec
//   * lastIndex
function IsRegExpMethodOptimizable(rx) {
    var RegExpProto = GetBuiltinPrototype("RegExp");
    // If RegExpPrototypeOptimizable and RegExpInstanceOptimizable succeed,
    // `RegExpProto.exec` is guaranteed to be data properties.
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpInstanceOptimizable(rx, RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec;
}

// ES 2016 draft Mar 25, 2016 21.2.5.8.
function RegExpReplace(string, replaceValue) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Step 4.
    var lengthS = S.length;

    // Step 5.
    var functionalReplace = IsCallable(replaceValue);

    // Step 6.
    var firstDollarIndex = -1;
    if (!functionalReplace) {
        // Step 6.a.
        replaceValue = ToString(replaceValue);
        firstDollarIndex = callFunction(std_String_indexOf, replaceValue, "$");
    }

    // Step 7.
    var global = !!rx.global;

    // Optimized paths for simple cases.
    if (!functionalReplace && firstDollarIndex === -1 && IsRegExpMethodOptimizable(rx)) {
        if (global) {
            if (lengthS < 0x7fff)
                return RegExpGlobalReplaceShortOpt(rx, S, lengthS, replaceValue);
            return RegExpGlobalReplaceOpt(rx, S, lengthS, replaceValue);
        }
        return RegExpLocalReplaceOpt(rx, S, lengthS, replaceValue);
    }

    // Step 8.
    var fullUnicode = false;
    if (global) {
        // Step 8.a.
        fullUnicode = !!rx.unicode;

        // Step 8.b.
        rx.lastIndex = 0;
    }

    // Step 9.
    var results = [];
    var nResults = 0;

    // Step 11.
    while (true) {
        // Step 11.a.
        var result = RegExpExec(rx, S, false);

        // Step 11.b.
        if (result === null)
            break;

        // Step 11.c.i.
        _DefineDataProperty(results, nResults++, result);

        // Step 11.c.ii.
        if (!global)
            break;

        // Step 11.c.iii.1.
        var matchStr = ToString(result[0]);

        // Step 11.c.iii.2.
        if (matchStr === "") {
            var lastIndex = ToLength(rx.lastIndex);
            rx.lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
        }
    }

    // Step 12.
    var accumulatedResult = "";

    // Step 13.
    var nextSourcePosition = 0;

    // Step 14.
    for (var i = 0; i < nResults; i++) {
        result = results[i];

        // Steps 14.a-b.
        var nCaptures = std_Math_max(ToLength(result.length) - 1, 0);

        // Step 14.c.
        var matched = ToString(result[0]);

        // Step 14.d.
        var matchLength = matched.length;

        // Steps 14.e-f.
        var position = std_Math_max(std_Math_min(ToInteger(result.index), lengthS), 0);

        var n, capN, replacement;
        if (functionalReplace || firstDollarIndex !== -1) {
            // Step 14.h.
            var captures = [];
            var capturesLength = 0;

            // Step 14.j.i (reordered).
            // For nCaptures <= 4 case, call replaceValue directly, otherwise
            // use std_Function_apply with all arguments stored in captures.
            // In latter case, store matched as the first element here, to
            // avoid unshift later.
            if (functionalReplace && nCaptures > 4)
                _DefineDataProperty(captures, capturesLength++, matched);

            // Step 14.g, 14.i, 14.i.iv.
            for (n = 1; n <= nCaptures; n++) {
                // Step 14.i.i.
                capN = result[n];

                // Step 14.i.ii.
                if (capN !== undefined)
                    capN = ToString(capN);

                // Step 14.i.iii.
                _DefineDataProperty(captures, capturesLength++, capN);
            }

            // Step 14.j.
            if (functionalReplace) {
                switch (nCaptures) {
                  case 0:
                    replacement = ToString(replaceValue(matched, position, S));
                    break;
                  case 1:
                    replacement = ToString(replaceValue(matched, captures[0], position, S));
                    break;
                  case 2:
                    replacement = ToString(replaceValue(matched, captures[0], captures[1],
                                                        position, S));
                    break;
                  case 3:
                    replacement = ToString(replaceValue(matched, captures[0], captures[1],
                                                        captures[2], position, S));
                    break;
                  case 4:
                    replacement = ToString(replaceValue(matched, captures[0], captures[1],
                                                        captures[2], captures[3], position, S));
                    break;
                  default:
                    // Steps 14.j.ii-v.
                    _DefineDataProperty(captures, capturesLength++, position);
                    _DefineDataProperty(captures, capturesLength++, S);
                    replacement = ToString(callFunction(std_Function_apply, replaceValue, null,
                                                        captures));
                }
            } else {
                // Steps 14.k.i.
                replacement = RegExpGetSubstitution(matched, S, position, captures, replaceValue,
                                                    firstDollarIndex);
            }
        } else {
            // Step 14.g, 14.i, 14.i.iv.
            // We don't need captures array, but ToString is visible to script.
            for (n = 1; n <= nCaptures; n++) {
                // Step 14.i.i-ii.
                capN = result[n];

                // Step 14.i.ii.
                if (capN !== undefined)
                    ToString(capN);
            }
            replacement = replaceValue;
        }

        // Step 14.l.
        if (position >= nextSourcePosition) {
            // Step 14.l.ii.
          accumulatedResult += Substring(S, nextSourcePosition,
                                         position - nextSourcePosition) + replacement;

            // Step 14.l.iii.
            nextSourcePosition = position + matchLength;
        }
    }

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}

// ES 2016 draft Mar 25, 2016 21.2.5.8 steps 8.a-16.
// Optimized path for @@replace with global flag, short string.
function RegExpGlobalReplaceShortOpt(rx, S, lengthS, replaceValue)
{
   // Step 8.a.
    var fullUnicode = !!rx.unicode;

    // Step 8.b.
    var lastIndex = 0;
    rx.lastIndex = 0;

    // Step 12 (reordered).
    var accumulatedResult = "";

    // Step 13 (reordered).
    var nextSourcePosition = 0;

    var sticky = !!rx.sticky;

    // Step 11.
    while (true) {
        // Step 11.a.
        var result = RegExpSearcher(rx, S, lastIndex, sticky);

        // Step 11.b.
        if (result === -1)
            break;

        var position = result & 0x7fff;
        lastIndex = (result >> 15) & 0x7fff;

        // Step 14.l.ii.
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replaceValue;

        // Step 14.l.iii.
        nextSourcePosition = lastIndex;

        // Step 11.c.iii.2.
        if (lastIndex === position) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
        }
    }

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}

// ES 2016 draft Mar 25, 2016 21.2.5.8 steps 8.a-16.
// Optimized path for @@replace with global flag.
function RegExpGlobalReplaceOpt(rx, S, lengthS, replaceValue)
{
   // Step 8.a.
    var fullUnicode = !!rx.unicode;

    // Step 8.b.
    var lastIndex = 0;
    rx.lastIndex = 0;

    // Step 12 (reordered).
    var accumulatedResult = "";

    // Step 13 (reordered).
    var nextSourcePosition = 0;

    var sticky = !!rx.sticky;

    // Step 11.
    while (true) {
        // Step 11.a.
        var result = RegExpMatcher(rx, S, lastIndex, sticky);

        // Step 11.b.
        if (result === null)
            break;

        // Step 11.c.iii.1.
        var matchStr = result[0];

        // Step 14.c.
        var matched = result[0];

        // Step 14.d.
        var matchLength = matched.length;

        // Steps 14.e-f.
        var position = result.index;
        lastIndex = position + matchLength;

        // Step 14.l.ii.
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replaceValue;

        // Step 14.l.iii.
        nextSourcePosition = lastIndex;

        // Step 11.c.iii.2.
        if (matchLength === 0) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
        }
    }

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}

// ES 2016 draft Mar 25, 2016 21.2.5.8 steps 11.a-16.
// Optimized path for @@replace without global flag.
function RegExpLocalReplaceOpt(rx, S, lengthS, replaceValue)
{
    var sticky = !!rx.sticky;

    var lastIndex;
    if (sticky) {
        lastIndex = ToLength(rx.lastIndex);
        if (lastIndex > lengthS) {
            rx.lastIndex = 0;
            return S;
        }
    } else {
        lastIndex = 0;
    }

    // Step 11.a.
    var result = RegExpMatcher(rx, S, lastIndex, sticky);

    // Step 11.b.
    if (result === null) {
        rx.lastIndex = 0;
        return S;
    }

    // Steps 11.c, 12-13, 14.a-b (skipped).

    // Step 14.c.
    var matched = result[0];

    // Step 14.d.
    var matchLength = matched.length;

    // Step 14.e-f.
    var position = result.index;

    // Step 14.l.ii.
    var accumulatedResult = Substring(S, 0, position) + replaceValue;

    // Step 14.l.iii.
    var nextSourcePosition = position + matchLength;

   if (sticky)
       rx.lastIndex = nextSourcePosition;

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}

// ES 2016 draft Mar 25, 2016 21.2.5.9.
function RegExpSearch(string) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    var result;
    if (IsRegExpMethodOptimizable(rx) && S.length < 0x7fff) {
        var sticky = !!rx.sticky;

        // Step 6.
        result = RegExpSearcher(rx, S, 0, sticky);

        // Step 8.
        if (result === -1)
            return -1;

        // Step 9.
        return result & 0x7fff;
    }

    // Step 4.
    var previousLastIndex = rx.lastIndex;

    // Step 5.
    rx.lastIndex = 0;

    // Step 6.
    result = RegExpExec(rx, S, false);

    // Step 7.
    rx.lastIndex = previousLastIndex;

    // Step 8.
    if (result === null)
        return -1;

    // Step 9.
    return result.index;
}

function IsRegExpSplitOptimizable(C) {
    var RegExpCtor = GetBuiltinConstructor("RegExp");
    if (C !== RegExpCtor)
        return false;

    var RegExpProto = RegExpCtor.prototype;
    // If RegExpPrototypeOptimizable succeeds, `RegExpProto.exec` is guaranteed
    // to be a data property.
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec;
}

// ES 2016 draft Mar 25, 2016 21.2.5.11.
function RegExpSplit(string, limit) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Step 4.
    var C = SpeciesConstructor(rx, GetBuiltinConstructor("RegExp"));

    // Step 5.
    var flags = ToString(rx.flags);

    // Steps 6-7.
    var unicodeMatching = callFunction(std_String_includes, flags, "u");

    // Steps 8-9.
    var newFlags;
    if (callFunction(std_String_includes, flags, "y"))
        newFlags = flags;
    else
        newFlags = flags + "y";

    // Step 10.
    var splitter = new C(rx, newFlags);

    // Step 11.
    var A = [];

    // Step 12.
    var lengthA = 0;

    // Step 13.
    var lim;
    if (limit === undefined)
        lim = MAX_NUMERIC_INDEX;
    else
        lim = limit >>> 0;

    // Step 14.
    var size = S.length;

    // Step 16;
    var p = 0;

    // Step 16;
    if (lim === 0)
        return A;

    // Step 17.
    if (size === 0) {
        // Step 17.a.
        var z = RegExpExec(splitter, S, false);

        // Step 17.b.
        if (z !== null)
            return A;

        // Step 17.d.
        _DefineDataProperty(A, 0, S);

        // Step 17.e.
        return A;
    }

    var optimizable = IsRegExpSplitOptimizable(C);

    // Step 18.
    var q = p;

    // Step 19.
    while (q < size) {
        var e;
        if (optimizable) {
            // Step 19.a (skipped).
            // splitter.lastIndex is not used.

            // Step 19.b.
            // Directly call RegExpMatcher to ignore flags and find first match.
            z = RegExpMatcher(splitter, S, q, false);

            // Step 19.c.
            if (z === null)
                break;

            // splitter.lastIndex is not updated.
            q = z.index;
            if (q >= size)
                break;

            // Step 19.d.i.
            e = ToLength(q + z[0].length);
        } else {
            // Step 19.a.
            splitter.lastIndex = q;

            // Step 19.b.
            z = RegExpExec(splitter, S, false);

            // Step 19.c.
            if (z === null) {
                q = unicodeMatching ? AdvanceStringIndex(S, q) : q + 1;
                continue;
            }

            // Step 19.d.i.
            e = ToLength(splitter.lastIndex);
        }

        // Step 19.d.iii.
        if (e === p) {
            q = unicodeMatching ? AdvanceStringIndex(S, q) : q + 1;
            continue;
        }

        // Steps 19.d.iv.1-3.
        _DefineDataProperty(A, lengthA, Substring(S, p, q - p));

        // Step 19.d.iv.4.
        lengthA++;

        // Step 19.d.iv.5.
        if (lengthA === lim)
            return A;

        // Step 19.d.iv.6.
        p = e;

        // Steps 19.d.iv.7-8.
        var numberOfCaptures = std_Math_max(ToLength(z.length) - 1, 0);

        // Step 19.d.iv.9.
        var i = 1;

        // Step 19.d.iv.10.
        while (i <= numberOfCaptures) {
            // Steps 19.d.iv.10.a-b.
            _DefineDataProperty(A, lengthA, z[i]);

            // Step 19.d.iv.10.c.
            i++;

            // Step 19.d.iv.10.d.
            lengthA++;

            // Step 19.d.iv.10.e.
            if (lengthA === lim)
                return A;
        }

        // Step 19.d.iv.11.
        q = p;
    }

    // Steps 20-22.
    if (p >= size)
        _DefineDataProperty(A, lengthA, "");
    else
        _DefineDataProperty(A, lengthA, Substring(S, p, size - p));

    // Step 23.
    return A;
}

// ES6 21.2.5.2.
// NOTE: This is not RegExpExec (21.2.5.2.1).
function RegExp_prototype_Exec(string) {
    // Steps 1-3.
    var R = this;
    if (!IsObject(R) || !IsRegExpObject(R))
        return callFunction(CallRegExpMethodIfWrapped, R, string, "RegExp_prototype_Exec");

    // Steps 4-5.
    var S = ToString(string);

    // Step 6.
    return RegExpBuiltinExec(R, S, false);
}

// ES6 21.2.5.2.1.
function RegExpExec(R, S, forTest) {
    // Steps 1-2 (skipped).

    // Steps 3-4.
    var exec = R.exec;

    // Step 5.
    // If exec is the original RegExp.prototype.exec, use the same, faster,
    // path as for the case where exec isn't callable.
    if (exec === RegExp_prototype_Exec || !IsCallable(exec)) {
        // ES6 21.2.5.2 steps 1-2, 4-5 (skipped) for optimized case.

        // Steps 6-7 or ES6 21.2.5.2 steps 3, 6 for optimized case.
        return RegExpBuiltinExec(R, S, forTest);
    }

    // Steps 5.a-b.
    var result = callContentFunction(exec, R, S);

    // Step 5.c.
    if (typeof result !== "object")
        ThrowTypeError(JSMSG_EXEC_NOT_OBJORNULL);

    // Step 5.d.
    return forTest ? result !== null : result;
}

// ES6 21.2.5.2.2.
function RegExpBuiltinExec(R, S, forTest) {
    // ES6 21.2.5.2.1 step 6.
    // This check is here for RegExpTest.  RegExp_prototype_Exec does same
    // thing already.
    if (!IsRegExpObject(R))
        return callFunction(CallRegExpMethodIfWrapped, R, R, S, forTest, "RegExpBuiltinExec");

    // Step 1-2 (skipped).

    // Steps 4-5.
    var lastIndex = ToLength(R.lastIndex);

    // Steps 6-7.
    var global = !!R.global;

    // Steps 8-9.
    var sticky = !!R.sticky;

    // Step 10.
    if (!global && !sticky) {
        lastIndex = 0;
    } else {
        if (lastIndex > S.length) {
            // Steps 15.a.i-ii, 15.c.i.1-2.
            R.lastIndex = 0;
            return forTest ? false : null;
        }
    }

    if (forTest) {
        // Steps 3, 11-17, except 15.a.i-ii, 15.c.i.1-2.
        var endIndex = RegExpTester(R, S, lastIndex, sticky);
        if (endIndex == -1) {
            // Steps 15.a.i-ii, 15.c.i.1-2.
            R.lastIndex = 0;
            return false;
        }

        // Step 18.
        if (global || sticky)
            R.lastIndex = endIndex;

        return true;
    }

    // Steps 3, 11-17, except 15.a.i-ii, 15.c.i.1-2.
    var result = RegExpMatcher(R, S, lastIndex, sticky);
    if (result === null) {
        // Steps 15.a.i-ii, 15.c.i.1-2.
        R.lastIndex = 0;
    } else {
        // Step 18.
        if (global || sticky)
            R.lastIndex = result.index + result[0].length;
    }

    return result;
}

// ES6 21.2.5.13.
function RegExpTest(string) {
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Steps 3-4.
    var S = ToString(string);

    // Steps 5-6.
    return RegExpExec(R, S, true);
}

// ES 2016 draft Mar 25, 2016 21.2.4.2.
function RegExpSpecies() {
    // Step 1.
    return this;
}
