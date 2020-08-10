%{
#include "postgres.h"
#include "knl/knl_variable.h"
#include "nodes/pg_list.h"
#include "nodes/nodes.h"
#include "parser/parse_hint.h"
#include "parser/parser.h"

#include "parser/gramparse.h"

#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"

extern void yyerror(yyscan_t yyscanner, const char *msg);
extern void hint_scanner_yyerror(const char *msg, yyscan_t yyscanner);

static Value *makeStringValue(char *str);

static Value *makeBitStringValue(char *str);

static Value *makeNullValue();

static Value *makeBoolValue(bool state);

#define YYMALLOC palloc
#define YYFREE   pfree


static double convert_to_numeric(Node *value);

%}

%pure-parser
%expect 0

%parse-param {yyscan_t yyscanner}
%lex-param   {yyscan_t yyscanner}


%union
{
	int		ival;
	char		*str;
	List		*list;
	Node	*node;
}



%type <node> join_hint_item join_order_hint join_method_hint stream_hint row_hint scan_hint skew_hint expr_const
%type <list> relation_list join_hint_list relation_item relation_list_with_p ident_list skew_relist
             column_list_p column_list value_list_p value_list value_list_item value_type value_list_with_bracket
%token <str>	IDENT FCONST SCONST BCONST XCONST
%token <ival>	ICONST

%token <keyword> NestLoop_P MergeJoin_P HashJoin_P No_P Leading_P Rows_P Broadcast_P
				Redistribute_P BlockName_P TableScan_P IndexScan_P IndexOnlyScan_P Skew_P NULL_P TRUE_P FALSE_P

%nonassoc	IDENT NULL_P

%%
//yacc syntax start here  
join_hint_list:
	join_hint_item join_hint_list
	{
		$$ = lcons($1, $2);
		u_sess->parser_cxt.hint_list = $$;
	}
	| /*EMPTY*/             { $$ = NIL; }
	;

join_hint_item:
	join_order_hint
	{
		$$ = $1;
	}
	| join_method_hint
	{
		$$ = $1;
	}
	| No_P join_method_hint
	{
		JoinMethodHint *joinHint = (JoinMethodHint *) $2;
		joinHint->negative = true;
		$$ = (Node *) joinHint;
	}
	| stream_hint
	{
		$$ = $1;
	}
	| No_P stream_hint
	{
		StreamHint	*streamHint = (StreamHint *) $2;
		streamHint->negative = true;
		$$ = (Node *) streamHint;
	}
	| row_hint
	{
		$$ = $1;
	}
	| BlockName_P '(' IDENT ')'
	{
		BlockNameHint *blockHint = makeNode(BlockNameHint);
		blockHint->base.relnames = list_make1(makeString($3));
		blockHint->base.hint_keyword = HINT_KEYWORD_BLOCKNAME;
		$$ = (Node *) blockHint;
	}
	| scan_hint
	{
		$$ = $1;
	}
	| No_P scan_hint
	{
		ScanMethodHint	*scanHint = (ScanMethodHint *) $2;
		scanHint->negative = true;
		$$ = (Node *) scanHint;
	}
	;
	| skew_hint
	{
		$$ = $1;
	}

join_order_hint:
	Leading_P '(' relation_list_with_p ')'
	{
		LeadingHint *leadingHint = makeNode(LeadingHint);
		leadingHint->base.relnames = $3;
		leadingHint->join_order_hint = true;
		leadingHint->base.hint_keyword = HINT_KEYWORD_LEADING;
		$$ = (Node *) leadingHint;
	}
	| Leading_P relation_list_with_p
	{
		LeadingHint *leadingHint = makeNode(LeadingHint);
		leadingHint->base.relnames = $2;
		leadingHint->base.hint_keyword = HINT_KEYWORD_LEADING;
		$$ = (Node *) leadingHint;
	}
	;

join_method_hint:
	NestLoop_P '(' ident_list ')'
	{
		JoinMethodHint *joinHint = makeNode(JoinMethodHint);
		joinHint->base.relnames = $3;
		joinHint->base.hint_keyword = HINT_KEYWORD_NESTLOOP;
		joinHint->base.state = HINT_STATE_NOTUSED;
		joinHint->joinrelids = NULL;
		joinHint->inner_joinrelids = NULL;

		$$ = (Node*)joinHint;
	}
	| MergeJoin_P '(' ident_list ')'
	{
		JoinMethodHint *joinHint = makeNode(JoinMethodHint);
		joinHint->base.relnames = $3;
		joinHint->base.hint_keyword = HINT_KEYWORD_MERGEJOIN;
		joinHint->base.state = HINT_STATE_NOTUSED;
		joinHint->joinrelids = NULL;
		joinHint->inner_joinrelids = NULL;

		$$ = (Node*)joinHint;
	}
	| HashJoin_P '(' ident_list ')'
	{
		JoinMethodHint *joinHint = makeNode(JoinMethodHint);
		joinHint->base.relnames = $3;
		joinHint->base.hint_keyword = HINT_KEYWORD_HASHJOIN;
		joinHint->base.state = HINT_STATE_NOTUSED;
		joinHint->joinrelids = NULL;
		joinHint->inner_joinrelids = NULL;

		$$ = (Node*)joinHint;
	}
	;

stream_hint:
	Broadcast_P '(' ident_list ')'
	{
		StreamHint *streamHint = makeNode(StreamHint);
		streamHint->base.relnames = $3;
		streamHint->base.hint_keyword = HINT_KEYWORD_BROADCAST;
		streamHint->stream_type = STREAM_BROADCAST;

		$$ = (Node*)streamHint;
	}
	| Redistribute_P '(' ident_list ')'
	{
		StreamHint *streamHint = makeNode(StreamHint);
		streamHint->base.relnames = $3;
		streamHint->base.hint_keyword = HINT_KEYWORD_REDISTRIBUTE;
		streamHint->stream_type = STREAM_REDISTRIBUTE;

		$$ = (Node*)streamHint;
	}
	;

row_hint:
	Rows_P '(' ident_list '#' expr_const ')'
	{
		RowsHint *rowHint = makeNode(RowsHint);
		rowHint->base.relnames = $3;
		rowHint->base.hint_keyword = HINT_KEYWORD_ROWS;
		rowHint->value_type = RVT_ABSOLUTE;
		rowHint->rows = convert_to_numeric($5);
		if (IsA($5, Float))
			rowHint->rows_str = strVal($5);

		$$ = (Node *) rowHint;
	}
	| Rows_P '(' ident_list '+' expr_const ')'
	{
		RowsHint *rowHint = makeNode(RowsHint);
		rowHint->base.relnames = $3;
		rowHint->base.hint_keyword = HINT_KEYWORD_ROWS;
		rowHint->value_type = RVT_ADD;
		rowHint->rows = convert_to_numeric($5);
		if (IsA($5, Float))
			rowHint->rows_str = strVal($5);

		$$ = (Node *) rowHint;
	}
	| Rows_P '(' ident_list '-' expr_const ')'
	{
		RowsHint *rowHint = makeNode(RowsHint);
		rowHint->base.relnames = $3;
		rowHint->base.hint_keyword = HINT_KEYWORD_ROWS;
		rowHint->value_type = RVT_SUB;
		rowHint->rows = convert_to_numeric($5);
		if (IsA($5, Float))
			rowHint->rows_str = strVal($5);

		$$ = (Node *) rowHint;
	}
	| Rows_P '(' ident_list '*' expr_const ')'
	{
		RowsHint *rowHint = makeNode(RowsHint);
		rowHint->base.relnames = $3;
		rowHint->base.hint_keyword = HINT_KEYWORD_ROWS;
		rowHint->value_type = RVT_MULTI;
		rowHint->rows = convert_to_numeric($5);
		if (IsA($5, Float))
			rowHint->rows_str = strVal($5);

		$$ = (Node *) rowHint;
	}
	;

scan_hint:
	TableScan_P '(' IDENT ')'
	{
		ScanMethodHint	*scanHint = makeNode(ScanMethodHint);
		scanHint->base.relnames = list_make1(makeString($3));
		scanHint->base.hint_keyword = HINT_KEYWORD_TABLESCAN;
		scanHint->base.state = HINT_STATE_NOTUSED;
		$$ = (Node *) scanHint;
	}
	|
	IndexScan_P '(' ident_list ')'
	{
		ScanMethodHint	*scanHint = makeNode(ScanMethodHint);
		scanHint->base.relnames = list_make1(linitial($3));
		scanHint->base.hint_keyword = HINT_KEYWORD_INDEXSCAN;
		scanHint->base.state = HINT_STATE_NOTUSED;
		scanHint->indexlist = list_delete_first($3);
		$$ = (Node *) scanHint;
	}
	|
	IndexOnlyScan_P '(' ident_list ')'
	{
		ScanMethodHint	*scanHint = makeNode(ScanMethodHint);
		scanHint->base.relnames = list_make1(linitial($3));
		scanHint->base.hint_keyword = HINT_KEYWORD_INDEXONLYSCAN;
		scanHint->base.state = HINT_STATE_NOTUSED;
		scanHint->indexlist = list_delete_first($3);
		$$ = (Node *) scanHint;
	}
	;

skew_hint:
	Skew_P '(' skew_relist column_list_p value_list_p ')'
	{
		SkewHint	*skewHint = makeNode(SkewHint);
		skewHint->base.relnames = $3;
		skewHint->base.hint_keyword = HINT_KEYWORD_SKEW;
		skewHint->base.state = HINT_STATE_NOTUSED;
		skewHint->column_list = $4;
		skewHint->value_list = $5;
		$$ = (Node *) skewHint;
	}
	|
	Skew_P '(' skew_relist column_list_p ')'
	{
		SkewHint	*skewHint = makeNode(SkewHint);
		skewHint->base.relnames = $3;
		skewHint->base.hint_keyword = HINT_KEYWORD_SKEW;
		skewHint->base.state = HINT_STATE_NOTUSED;
		skewHint->column_list = $4;
		skewHint->value_list = NIL;
		$$ = (Node *) skewHint;
	}
	;

relation_list_with_p:
	'(' relation_list ')'           { $$ = $2; }
	;

relation_item:
	IDENT                           { $$ = list_make1(makeString($1)); }
	| relation_list_with_p          { $$ = list_make1($1); }
	;

relation_list:
	relation_item relation_item       { $$ = list_concat($1, $2); }
	| relation_list relation_item       { $$ = list_concat($1, $2); }
	;

ident_list:
	IDENT				{ $$ = list_make1(makeString($1)); }
	| ident_list IDENT		{ $$ = lappend($1, makeString($2)); }
	;

expr_const:
	ICONST				{ $$ = (Node *) makeInteger($1); }
	| FCONST			{ $$ = (Node *) makeFloat($1); }
	;	

skew_relist:
	IDENT					{ $$ = list_make1(makeString($1)); }
	|'(' ident_list ')'		{ $$ = $2; }
	;

column_list_p:
	'(' column_list ')'	{ $$ = $2; }
	;

column_list:
	IDENT					{ $$ = list_make1(makeString($1)); }
	| column_list IDENT		{ $$ = lappend($1, makeString($2)); }
	;

value_list_p:
	'(' value_list ')'     { $$ = $2; }
	;

value_list:
	value_list_item				   { $$ = $1; }
	| value_list_with_bracket      { $$ = $1; }
	;

value_list_with_bracket:
	value_list_p		{ $$ = $1; }
	| value_list_with_bracket value_list_p   { $$ = list_concat($1, $2); }
	;

value_list_item:
	value_type			{ $$ = $1; }
	| value_list_item value_type		{$$ = list_concat($1, $2); }
	;

value_type:
	ICONST				{ $$ = list_make1(makeInteger($1)); }
	| FCONST			{ $$ = list_make1(makeFloat($1)); }
	| SCONST			{ $$ = list_make1(makeStringValue($1)); } /* specially process for null value. */
	| BCONST 			{ $$ = list_make1(makeBitStringValue($1)); } /* for bit string litera*/
	| XCONST 			{ $$ = list_make1(makeString($1)); }  /* hexadecimal numeric string*/
	| NULL_P			{ $$ = list_make1(makeNullValue()); }
	| TRUE_P			{ $$ = list_make1(makeBoolValue(TRUE)); } /* for boolean type, we save as string with type T_String. */
	| FALSE_P			{ $$ = list_make1(makeBoolValue(FALSE)); }
	;
%%

void
 yyerror(yyscan_t yyscanner, const char *msg)
{
	hint_scanner_yyerror(msg, yyscanner);
	return;
}

static double
convert_to_numeric(Node *value)
{
	double	d = 0;
	Value	*vvalue = (Value *) value;

	switch(nodeTag(vvalue))
	{
		case T_Integer:
			d = intVal(vvalue);
			break;
		case T_Float:
			d = floatVal(vvalue);
			break;
		default:
			break;
	}

	return d;
}

static Value *
makeStringValue(char *str)
{
	Value	   *val = makeNode(Value);

	if (DB_IS_CMPT(DB_CMPT_A))
	{
		if (NULL == str || 0 == strlen(str))
		{
			val->type = T_Null;
			val->val.str = str;
		}
		else
		{
			val->type = T_String;
			val->val.str = str;
		}
	}
	else
	{
		val->type = T_String;
		val->val.str = str;
	}

	return val;
}

static Value *
makeBitStringValue(char *str)
{
	Value	*val = makeNode(Value);

	val->type = T_BitString;
	val->val.str = str;

	return val;
}

static Value *
makeNullValue()
{
	Value	*val = makeNode(Value);

	val->type = T_Null;

	return val;

}

static Value *
makeBoolValue(bool state)
{
	Value	*val = makeNode(Value);
	val->type = T_String;
	val->val.str = (char *)(state ? "t" : "f");

	return val;
}


#include "hint_scan.cpp"

