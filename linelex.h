/*
MIT License

Copyright (c) 2025 virtualgrub39 virtualgrub39(at)tutamail.com

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef _LINELEX_H
#define _LINELEX_H

#define LINELEX_API_V 1

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>

typedef int LL_TokenID;

typedef struct
{
    const char *ptr;
    size_t len;
    LL_TokenID type;
    size_t type_idx;
} LL_Token;
typedef LL_Token *LL_TokenArray;

typedef struct
{
    LL_TokenID id;
    const char *name;
    const char *rgx_pattern;
    bool skip;
    regex_t compiled;
} LL_TokenType;
typedef LL_TokenType *LL_TokenTypeArray;

#define LL_TokenTypeDef(d_id, hname, pattern, d_skip)                                              \
    (LL_TokenType)                                                                                 \
    {                                                                                              \
        .id = d_id, .name = hname, .rgx_pattern = pattern, .skip = d_skip, .compiled = { 0 }       \
    }

typedef struct
{
    bool case_insensitive;
    bool auto_anchor;
    bool use_extended_regex;
    size_t max_tokens;
} LL_LexerConfig;

typedef struct
{
    LL_LexerConfig cfg;
    LL_TokenTypeArray token_types;
} LL_Lexer;

void LL_lexer_initEx (LL_Lexer *lexer, LL_LexerConfig cfg);
ssize_t LL_token_type_add (LL_Lexer *lexer, LL_TokenID id, const char *human_name,
                           const char *pattern, bool skip);
ssize_t LL_lexer_lex (LL_Lexer *lexer, const char *input, LL_TokenArray *out);
void LL_lexer_cleanup (LL_Lexer *lexer);

#ifdef LINELEX_SHORT_NAMES
typedef LL_TokenID TokenID;
typedef LL_Lexer Lexer;
typedef LL_LexerConfig LexerConfig;
typedef LL_TokenType TokenType;
typedef LL_TokenTypeArray TokenTypeArray;
typedef LL_Token Token;
typedef LL_TokenArray TokenArray;

#define lexer_initEx(lexer, cfg) LL_lexer_initEx (lexer, cfg)
#define token_type_add(lexer, id, name, pattern, skip)                                             \
    LL_token_type_add (lexer, id, name, pattern, skip)
#define lexer_lex(lexer, input, output) LL_lexer_lex (lexer, input, output)
#define lexer_cleanup(lexer) LL_lexer_cleanup (lexer)

#define lexer_init(lexer, ...) LL_lexer_initEx (lexer, (LL_LexerConfig){ __VA_ARGS__ })
#define token_type_adds(lexer, token_type)                                                         \
    LL_token_type_add (lexer, token_type.id, token_type.name, token_type.rgx_pattern,              \
                       token_type.skip)

#define TokenTypeDef LL_TokenTypeDef

#else
#define LL_lexer_init(lexer, ...) LL_lexer_initEx (lexer, (LL_LexerConfig){ __VA_ARGS__ })
#define LL_token_type_adds(lexer, token_type)                                                      \
    LL_token_type_add (lexer, token_type.id, token_type.name, token_type.rgx_pattern,              \
                       token_type.skip)
#endif

#ifdef LINELEX_IMPLEMENTATION

#include <errno.h>
#ifndef STB_DS_IMPLEMENTATION
#include "stb_ds.h"
#endif

void
LL_lexer_initEx (LL_Lexer *lexer, LL_LexerConfig cfg)
{
    lexer->cfg.case_insensitive = cfg.case_insensitive;
    lexer->cfg.auto_anchor = cfg.auto_anchor;
    lexer->cfg.max_tokens = cfg.max_tokens;
    lexer->cfg.use_extended_regex = cfg.use_extended_regex;
    lexer->token_types = NULL;
}

ssize_t
LL_token_type_add (LL_Lexer *lexer, LL_TokenID id, const char *human_name, const char *pattern,
                   bool skip)
{
    if (!lexer || !pattern)
    {
        errno = EFAULT;
        return -1;
    }

    size_t prev_len = arrlen (lexer->token_types);

    if (id < 0)
    {
        long last = (long)prev_len - 1;
        if (last <= 0)
            id = 0;
        else
            id = lexer->token_types[last].id;
    }

    int flags = 0;
    if (lexer->cfg.case_insensitive)
        flags |= REG_ICASE;
    if (lexer->cfg.use_extended_regex)
        flags |= REG_EXTENDED;

    char *relpattern = (char *)pattern;
    if (lexer->cfg.auto_anchor)
    {
        relpattern = malloc (strlen (pattern + 2));
        memcpy (relpattern + 1, pattern, strlen (pattern) + 1);
        relpattern[0] = '^';
    }

    regex_t compiled;
    int regresult = regcomp (&compiled, relpattern, flags);
    if (regresult != 0)
    {
        errno = EINVAL;
        return (size_t)regresult;
    }

    if (lexer->cfg.auto_anchor)
        free (relpattern);

    LL_TokenType tt = {
        .id = id, .name = human_name, .rgx_pattern = pattern, .skip = skip, .compiled = compiled
    };

    arrput (lexer->token_types, tt);

    return 0;
}

ssize_t
LL_lexer_lex (LL_Lexer *lexer, const char *input, LL_TokenArray *out)
{
    if (!input || !lexer)
    {
        errno = EFAULT;
        return -1;
    }

    const char *pos = input;

    while (*pos != '\0')
    {
        bool found_match = false;
        for (long i = 0; i < arrlen (lexer->token_types); ++i)
        {
            regmatch_t match;
            LL_TokenType *type = &lexer->token_types[i];

            // test_regex_matching (type->rgx_pattern, pos); // DEBUG

            if (regexec (&type->compiled, pos, 1, &match, 0) == 0 && match.rm_so == 0)
            {
                // printf ("DEBUG: pos='%.10s', pattern='%s', rm_so=%d, rm_eo=%d\n", pos,
                //         type->rgx_pattern, (int)match.rm_so, (int)match.rm_eo);

                if (match.rm_eo <= 0)
                {
                    errno = EINVAL;
                    // fprintf (stderr, "LL_LexEx: zero-length match for pattern '%s' at pos
                    // '%s'\n",
                    //          type->rgx_pattern, pos);
                    return -1;
                }

                if (!type->skip)
                {
                    if (lexer->cfg.max_tokens != 0 && (long)lexer->cfg.max_tokens <= arrlen (*out))
                    {
                        errno = EPERM;
                        return -1;
                    }

                    LL_Token token = {
                        .ptr = pos,
                        .len = (size_t)match.rm_eo,
                        .type = type->id,
                        .type_idx = i,
                    };
                    arrput (*out, token);
                }

                pos += match.rm_eo;
                found_match = true;
                break; /* first match wins */
            }
        }

        if (!found_match)
        {
            errno = EINVAL;
            return -1;
        }
    }

    return arrlen (*out);
}

void
LL_lexer_cleanup (LL_Lexer *lexer)
{
    if (lexer == NULL)
        return;

    if (lexer->token_types)
    {
        size_t n = arrlen (lexer->token_types);
        for (size_t i = 0; i < n; ++i)
        {
            regfree (&lexer->token_types[i].compiled);
        }
        arrfree (lexer->token_types);
        lexer->token_types = NULL;
    }
}

#endif

// #define LL_PATTERN_IDENTIFIER "^[a-zA-Z_][a-zA-Z0-9_]*"
// #define LL_PATTERN_INTEGER "^[0-9]+"
// #define LL_PATTERN_STRING_DQ "^\"([^\"\\\\]|\\\\.)*\""
// #define LL_PATTERN_FLOAT "^[0-9]*\\.[0-9]+([eE][+-]?[0-9]+)?"
// #define LL_PATTERN_STRING_SQ "^'([^'\\\\]|\\\\.)*'"
// #define LL_PATTERN_WHITESPACE "^[\t\r\n ]+"

#endif

// TODO: Documentation
// TODO: Fix naming
// TODO: More backends
// TODO: Config option for precedence ?
// TODO: Revamp config - related to backends
