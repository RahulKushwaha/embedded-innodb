/*****************************************************************************

Copyright (c) 1997, 2009, Innobase Oy. All Rights Reserved.
Copyright (c) 2024, Sunny Bains. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/******************************************************
SQL parser lexical analyzer: input file for the GNU Flex lexer generator

The InnoDB parser is frozen because MySQL takes care of SQL parsing.
Therefore we normally keep the InnoDB parser C files as they are, and do
not automatically generate them from pars0grm.y and pars0lex.l.

How to make the InnoDB parser and lexer C files:

1. Run ./make_flex.sh to generate lexer files.

2. Run ./make_bison.sh to generate parser files.

These instructions seem to work at least with bison-1.875d and flex-2.5.31 on
Linux.

Created 12/14/1997 Heikki Tuuri
*******************************************************/

%option nostdinit
%option 8bit
%option warn
%option pointer
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyy_scan_buffer
%option noyy_scan_bytes
%option noyy_scan_string
%option nounistd
%option c++

%{
#define YYSTYPE que_node_t*

#include "pars0pars.h"
#include "pars0grm.h"
#include "pars0sym.h"
#include "mem0mem.h"
#include "os0proc.h"

#define malloc(A)      ut_new(A)
#define free(A)        ut_delete(A)
#define realloc(P, A)  ut_realloc(P, A)
#define exit(A)        ut_error

/* Note: We cast result from int to yy_size_t */
#define YY_INPUT(buf, result, max_size) ((result) = pars_get_lex_chars(buf, max_size))

/* String buffer for removing quotes */

/* Allocated length */
static ulint stringbuf_len_alloc = 0;

/* Current length */
static ulint stringbuf_len = 0;

/* Start of buffer */
static char* stringbuf;

static yyFlexLexer *pars_scanner{};

/**
 * Appends a string to the buffer.
 *
 * @param[in] str String to be appended
 * @param[in] len Length of string.
 */
static void string_append(const char* str, ulint len) {

  // Disable compiler warning about unused variable.
  (void) yyleng;

  if (stringbuf == NULL) {
    stringbuf = (char*) ut_new(1);
    stringbuf_len_alloc = 1;
  }

  if (stringbuf_len + len > stringbuf_len_alloc) {
    while (stringbuf_len + len > stringbuf_len_alloc) {
      stringbuf_len_alloc <<= 1;
    }
    stringbuf = (char*) ut_realloc(stringbuf, stringbuf_len_alloc);
  }

  memcpy(stringbuf + stringbuf_len, str, len);
  stringbuf_len += len;
}

void pars_lexer_var_init() {
  stringbuf_len = 0;
  stringbuf_len_alloc = 0;

  if (stringbuf != NULL) {
    ut_delete(stringbuf);
    stringbuf = NULL;
  }
}

void create_pars_scanner() {
 ut_a(pars_scanner == nullptr);
 pars_scanner = new yyFlexLexer();
}

%}

DIGIT  [0-9]
ID  [a-z_A-Z][a-z_A-Z0-9]*
BOUND_LIT  \:[a-z_A-Z0-9]+
BOUND_ID  \$[a-z_A-Z0-9]+

%x comment
%x quoted
%x id
%%

{DIGIT}+  {
      yylval = sym_tab_add_int_lit(pars_sym_tab_global, atoi(YYText()));
      return(PARS_INT_LIT);
}

{DIGIT}+"."{DIGIT}* {
      ut_error;  /* not implemented */

      return(PARS_FLOAT_LIT);
}

{BOUND_LIT}  {
      ulint  type;

      yylval = sym_tab_add_bound_lit(pars_sym_tab_global, YYText() + 1, &type);

      return((int) type);
}

{BOUND_ID}  {
      yylval = sym_tab_add_bound_id(pars_sym_tab_global, YYText() + 1);

      return(PARS_ID_TOKEN);
}

"'"    {
/* Quoted character string literals are handled in an explicit
start state 'quoted'.  This state is entered and the buffer for
the scanned string is emptied upon encountering a starting quote.

In the state 'quoted', only two actions are possible (defined below). */
      BEGIN(quoted);
      stringbuf_len = 0;
}
<quoted>[^\']+  {
      /* Got a sequence of characters other than "'":
      append to string buffer */
      string_append(YYText(), YYLeng());
}
<quoted>"'"+  {
      /* Got a sequence of "'" characters:
      append half of them to string buffer,
      as "''" represents a single "'".
      We apply truncating division,
      so that "'''" will result in "'". */

      string_append(YYText(), YYLeng() / 2);

      /* If we got an odd number of quotes, then the
      last quote we got is the terminating quote.
      At the end of the string, we return to the
      initial start state and report the scanned
      string literal. */

      if (YYLeng() % 2) {
        BEGIN(INITIAL);
        yylval = sym_tab_add_str_lit(
          pars_sym_tab_global,
          (byte*) stringbuf, stringbuf_len);
        return(PARS_STR_LIT);
      }
}

\"    {
/* Quoted identifiers are handled in an explicit start state 'id'.
This state is entered and the buffer for the scanned string is emptied
upon encountering a starting quote.

In the state 'id', only two actions are possible (defined below). */
      BEGIN(id);
      stringbuf_len = 0;
}
<id>[^\"]+  {
      /* Got a sequence of characters other than '"':
      append to string buffer */
      string_append(YYText(), YYLeng());
}
<id>\"+  {
      /* Got a sequence of '"' characters:
      append half of them to string buffer,
      as '""' represents a single '"'.
      We apply truncating division,
      so that '"""' will result in '"'. */

      string_append(YYText(), YYLeng() / 2);

      /* If we got an odd number of quotes, then the
      last quote we got is the terminating quote.
      At the end of the string, we return to the
      initial start state and report the scanned
      identifier. */

      if (YYLeng() % 2) {
        BEGIN(INITIAL);
        yylval = sym_tab_add_id(
          pars_sym_tab_global,
          (byte*) stringbuf, stringbuf_len);

        return(PARS_ID_TOKEN);
      }
}

"NULL"    {
      yylval = sym_tab_add_null_lit(pars_sym_tab_global);

      return(PARS_NULL_LIT);
}

"SQL"    {
      /* Implicit cursor name */
      yylval = sym_tab_add_str_lit(pars_sym_tab_global, (byte*) YYText(), YYLeng());
      return(PARS_SQL_TOKEN);
}

"AND"    {
      return(PARS_AND_TOKEN);
}

"OR"    {
      return(PARS_OR_TOKEN);
}

"NOT"    {
      return(PARS_NOT_TOKEN);
}

"PROCEDURE"  {
      return(PARS_PROCEDURE_TOKEN);
}

"IN"    {
      return(PARS_IN_TOKEN);
}

"OUT"    {
      return(PARS_OUT_TOKEN);
}

"BINARY"  {
       return(PARS_BINARY_TOKEN);
}

"BLOB"    {
       return(PARS_BLOB_TOKEN);
}

"INT"    {
       return(PARS_INT_TOKEN);
}

"INTEGER"  {
       return(PARS_INT_TOKEN);
}

"FLOAT"    {
       return(PARS_FLOAT_TOKEN);
}

"CHAR"    {
       return(PARS_CHAR_TOKEN);
}

"IS"    {
      return(PARS_IS_TOKEN);
}

"BEGIN"    {
      return(PARS_BEGIN_TOKEN);
}

"END"    {
      return(PARS_END_TOKEN);
}

"IF"    {
      return(PARS_IF_TOKEN);
}

"THEN"    {
      return(PARS_THEN_TOKEN);
}

"ELSE"    {
      return(PARS_ELSE_TOKEN);
}

"ELSIF"    {
      return(PARS_ELSIF_TOKEN);
}

"LOOP"    {
      return(PARS_LOOP_TOKEN);
}

"WHILE"    {
      return(PARS_WHILE_TOKEN);
}

"RETURN"  {
      return(PARS_RETURN_TOKEN);
}

"SELECT"  {
      return(PARS_SELECT_TOKEN);
}

"SUM"    {
      return(PARS_SUM_TOKEN);
}

"COUNT"    {
      return(PARS_COUNT_TOKEN);
}

"DISTINCT"  {
      return(PARS_DISTINCT_TOKEN);
}

"FROM"    {
      return(PARS_FROM_TOKEN);
}

"WHERE"    {
      return(PARS_WHERE_TOKEN);
}

"FOR"    {
      return(PARS_FOR_TOKEN);
}

"READ"    {
      return(PARS_READ_TOKEN);
}

"ORDER"    {
      return(PARS_ORDER_TOKEN);
}

"BY"    {
      return(PARS_BY_TOKEN);
}

"ASC"    {
      return(PARS_ASC_TOKEN);
}

"DESC"    {
      return(PARS_DESC_TOKEN);
}

"INSERT"  {
      return(PARS_INSERT_TOKEN);
}

"INTO"    {
      return(PARS_INTO_TOKEN);
}

"VALUES"  {
      return(PARS_VALUES_TOKEN);
}

"UPDATE"  {
      return(PARS_UPDATE_TOKEN);
}

"SET"    {
      return(PARS_SET_TOKEN);
}

"DELETE"  {
      return(PARS_DELETE_TOKEN);
}

"CURRENT"  {
      return(PARS_CURRENT_TOKEN);
}

"OF"    {
      return(PARS_OF_TOKEN);
}

"CREATE"  {
      return(PARS_CREATE_TOKEN);
}

"TABLE"    {
      return(PARS_TABLE_TOKEN);
}

"INDEX"    {
       return(PARS_INDEX_TOKEN);
}

"UNIQUE"  {
       return(PARS_UNIQUE_TOKEN);
}

"CLUSTERED"  {
       return(PARS_CLUSTERED_TOKEN);
}

"DOES_NOT_FIT_IN_MEMORY"  {
      return(PARS_DOES_NOT_FIT_IN_MEM_TOKEN);
}

"ON"    {
       return(PARS_ON_TOKEN);
}

"DECLARE"  {
      return(PARS_DECLARE_TOKEN);
}

"CURSOR"  {
      return(PARS_CURSOR_TOKEN);
}

"OPEN"  {
      return(PARS_OPEN_TOKEN);
}

"FETCH"  {
      return(PARS_FETCH_TOKEN);
}

"CLOSE"  {
      return(PARS_CLOSE_TOKEN);
}

"NOTFOUND"  {
      return(PARS_NOTFOUND_TOKEN);
}

"TO_CHAR"  {
      return(PARS_TO_CHAR_TOKEN);
}

"TO_NUMBER"  {
      return(PARS_TO_NUMBER_TOKEN);
}

"TO_BINARY"  {
      return(PARS_TO_BINARY_TOKEN);
}

"BINARY_TO_NUMBER" {
      return(PARS_BINARY_TO_NUMBER_TOKEN);
}

"SUBSTR"  {
      return(PARS_SUBSTR_TOKEN);
}

"REPLSTR"  {
      return(PARS_REPLSTR_TOKEN);
}

"CONCAT"  {
      return(PARS_CONCAT_TOKEN);
}

"INSTR"    {
      return(PARS_INSTR_TOKEN);
}

"LENGTH"  {
      return(PARS_LENGTH_TOKEN);
}

"SYSDATE"  {
      return(PARS_SYSDATE_TOKEN);
}

"PRINTF"  {
      return(PARS_PRINTF_TOKEN);
}

"ASSERT"  {
      return(PARS_ASSERT_TOKEN);
}

"RND"    {
      return(PARS_RND_TOKEN);
}

"RND_STR"  {
      return(PARS_RND_STR_TOKEN);
}

"ROW_PRINTF"  {
      return(PARS_ROW_PRINTF_TOKEN);
}

"COMMIT"  {
      return(PARS_COMMIT_TOKEN);
}

"ROLLBACK"  {
      return(PARS_ROLLBACK_TOKEN);
}

"WORK"    {
      return(PARS_WORK_TOKEN);
}

"UNSIGNED"  {
      return(PARS_UNSIGNED_TOKEN);
}

"EXIT"    {
      return(PARS_EXIT_TOKEN);
}

"FUNCTION"  {
      return(PARS_FUNCTION_TOKEN);
}

"LOCK"  {
      return(PARS_LOCK_TOKEN);
}

"SHARE"  {
      return(PARS_SHARE_TOKEN);
}

"MODE"  {
      return(PARS_MODE_TOKEN);
}

{ID}    {
      yylval = sym_tab_add_id(pars_sym_tab_global,
              (byte*)YYText(),
              strlen(YYText()));
      return(PARS_ID_TOKEN);
}

".."    {
      return(PARS_DDOT_TOKEN);
}

":="    {
      return(PARS_ASSIGN_TOKEN);
}

"<="    {
      return(PARS_LE_TOKEN);
}

">="    {
      return(PARS_GE_TOKEN);
}

"<>"    {
      return(PARS_NE_TOKEN);
}

"("    {

      return((int)(*YYText()));
}

"="    {

      return((int)(*YYText()));
}

">"    {

      return((int)(*YYText()));
}

"<"    {

      return((int)(*YYText()));
}

","    {

      return((int)(*YYText()));
}

";"    {

      return((int)(*YYText()));
}

")"    {

      return((int)(*YYText()));
}

"+"     {

      return((int)(*YYText()));
}

"-"    {

      return((int)(*YYText()));
}

"*"    {

      return((int)(*YYText()));
}

"/"    {

      return((int)(*YYText()));
}

"%"    {

      return((int)(*YYText()));
}

"{"    {

      return((int)(*YYText()));
}

"}"    {

      return((int)(*YYText()));
}

"?"    {

      return((int)(*YYText()));
}

"/*"      BEGIN(comment); /* eat up comment */

<comment>[^*]*
<comment>"*"+[^*/]*
<comment>"*"+"/"        BEGIN(INITIAL);

[ \t\n]+    /* eat up whitespace */


.    {
      log_err(std::format("Unrecognized character: {:x}\n", *YYText()));

      ut_error;

      return(0);
}

<<EOF>> {
  pars_lexer_close();
  return 0;
}

%%

extern "C" int yylex() {
  if (pars_scanner == nullptr) {
    create_pars_scanner();
  }
  return pars_scanner->yylex();
}

/**
 * Release any resources used by the lexer.
 */
void pars_lexer_close() {
  delete pars_scanner;
  pars_scanner = nullptr;

  if (stringbuf != nullptr) {
    free(stringbuf);
    stringbuf = nullptr;
  }

  stringbuf_len_alloc = stringbuf_len = 0;
}
