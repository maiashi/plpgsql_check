/*-------------------------------------------------------------------------
 *
 * expr_walk.c
 *
 *			  set of Query/Expr walkers
 *
 * by Pavel Stehule 2013-2018
 *
 *-------------------------------------------------------------------------
 */

#include "plpgsql_check.h"

#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

typedef struct 
{
	PLpgSQL_checkstate *cstate;
	char *query_str;
} check_funcexpr_walker_params;

static int check_fmt_string(const char *fmt,
							List *args,
							int location,
							check_funcexpr_walker_params *wp,
							bool *is_error,
							int *unsafe_expr_location);

/*
 * Send to ouput all not yet displayed relations and functions.
 */
static bool
detect_dependency_walker(Node *node, void *context)
{
	PLpgSQL_checkstate *cstate = (PLpgSQL_checkstate *) context;
	plpgsql_check_result_info *ri = cstate->result_info;

	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		ListCell *lc;

		foreach(lc, query->rtable)
		{
			RangeTblEntry *rt = (RangeTblEntry *) lfirst(lc);

			if (rt->rtekind == RTE_RELATION)
			{
				if (!bms_is_member(rt->relid, cstate->rel_oids))
				{
					plpgsql_check_put_dependency(ri,
												 "RELATION",
												 rt->relid,
												 get_namespace_name(get_rel_namespace(rt->relid)),
												 get_rel_name(rt->relid),
												 NULL);

					cstate->rel_oids = bms_add_member(cstate->rel_oids, rt->relid);
				}
			}
		}

		return query_tree_walker((Query *) node,
								 detect_dependency_walker,
								 context, 0);
	}

	if (IsA(node, FuncExpr) )
	{
		FuncExpr *fexpr = (FuncExpr *) node;

		if (get_func_namespace(fexpr->funcid) != PG_CATALOG_NAMESPACE)
		{
			if (!bms_is_member(fexpr->funcid, cstate->func_oids))
			{
				StringInfoData		str;
				ListCell		   *lc;
				int		i = 0;

				initStringInfo(&str);
				appendStringInfoChar(&str, '(');
				foreach(lc, fexpr->args)
				{
					Node *expr = (Node *) lfirst(lc);

					if (i++ > 0)
						appendStringInfoChar(&str, ',');

					appendStringInfoString(&str, format_type_be(exprType(expr)));
				}
				appendStringInfoChar(&str, ')');

				plpgsql_check_put_dependency(ri,
											  "FUNCTION",
											  fexpr->funcid,
											  get_namespace_name(get_func_namespace(fexpr->funcid)),
											  get_func_name(fexpr->funcid),
											  str.data);

				pfree(str.data);

				cstate->func_oids = bms_add_member(cstate->func_oids, fexpr->funcid);
			}
		}
	}

	return expression_tree_walker(node, detect_dependency_walker, context);
}

void
plpgsql_check_detect_dependency(PLpgSQL_checkstate *cstate, Query *query)
{
	if (cstate->result_info->format != PLPGSQL_SHOW_DEPENDENCY_FORMAT_TABULAR)
		return;

	detect_dependency_walker((Node *) query, cstate);
}

/*
 * Expecting persistent oid of nextval, currval and setval functions.
 * Ensured by regress tests.
 */
#define NEXTVAL_OID		1574
#define CURRVAL_OID		1575
#define SETVAL_OID		1576
#define SETVAL2_OID		1765
#define FORMAT_0PARAM_OID	3540
#define FORMAT_NPARAM_OID	3539


/*
 * When sequence related functions has constant oid parameter, then ensure, so
 * this oid is related to some sequnce object.
 *
 */
static bool
check_funcexpr_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		return query_tree_walker((Query *) node,
								 check_funcexpr_walker,
								 context, 0);
	}
	if (IsA(node, FuncExpr))
	{
		FuncExpr *fexpr = (FuncExpr *) node;
		int		location = -1;

		switch (fexpr->funcid)
		{
			case NEXTVAL_OID:
			case CURRVAL_OID:
			case SETVAL_OID:
			case SETVAL2_OID:
				{
					Node *first_arg = linitial(fexpr->args);

					location = fexpr->location;

					if (first_arg && IsA(first_arg, Const))
					{
						Const *c = (Const *) first_arg;

						if (c->consttype == REGCLASSOID && !c->constisnull)
						{
							Oid		classid;

							if (c->location != -1)
								location = c->location;

							classid = DatumGetObjectId(c->constvalue);

							if (get_rel_relkind(classid) != RELKIND_SEQUENCE)
							{
								char	message[1024];
								check_funcexpr_walker_params *wp;

								wp = (check_funcexpr_walker_params *) context;

								snprintf(message, sizeof(message), "\"%s\" is not a sequence", get_rel_name(classid));

								plpgsql_check_put_error(wp->cstate,
														ERRCODE_WRONG_OBJECT_TYPE, 0,
					  									message,
														NULL, NULL,
														PLPGSQL_CHECK_ERROR,
														location,
														wp->query_str, NULL);
							}
						}
					}
				}
				break;

			case FORMAT_0PARAM_OID:
			case FORMAT_NPARAM_OID:
				{
					/* We can do check only when first argument is constant */
					Node *first_arg = linitial(fexpr->args);

					if (first_arg && IsA(first_arg, Const))
					{
						Const *c = (Const *) first_arg;

						if (c->consttype == TEXTOID && !c->constisnull)
						{
							char *fmt = TextDatumGetCString(c->constvalue);
							check_funcexpr_walker_params *wp;
							int		required_nargs;
							bool	is_error;

							wp = (check_funcexpr_walker_params *) context;

							required_nargs = check_fmt_string(fmt, fexpr->args, c->location, wp, &is_error, NULL);
							if (!is_error && required_nargs != -1)
							{
								if (required_nargs + 1 != list_length(fexpr->args))
									plpgsql_check_put_error(wp->cstate,
															0, 0,
															"unused parameters of function \"format\"",
															NULL, NULL,
															PLPGSQL_CHECK_WARNING_OTHERS,
															c->location,
															wp->query_str, NULL);
							}
						}
					}
				}
		}
	}

	return expression_tree_walker(node, check_funcexpr_walker, context);
}

void
plpgsql_check_funcexpr(PLpgSQL_checkstate *cstate, Query *query, char *query_str)
{
	check_funcexpr_walker_params  wp;

	wp.cstate = cstate;
	wp.query_str = query_str;

	check_funcexpr_walker((Node *) query, &wp);
}

/*
 * Aux functions for checking format string of function format
 */

/*
 * Support macros for text_format()
 */
#define ADVANCE_PARSE_POINTER(ptr,end_ptr) \
	do { \
		if (++(ptr) >= (end_ptr)) \
		{ \
			if (wp) \
				plpgsql_check_put_error(wp->cstate, \
										ERRCODE_INVALID_PARAMETER_VALUE, 0, \
										"unterminated format() type specifier", \
										NULL, \
										"For a single \"%%\" use \"%%%%\".", \
										PLPGSQL_CHECK_ERROR, \
										location, \
										wp->query_str, NULL); \
			*is_error = true; \
		} \
	} while (0)

/*
 * Parse contiguous digits as a decimal number.
 *
 * Returns true if some digits could be parsed.
 * The value is returned into *value, and *ptr is advanced to the next
 * character to be parsed.
 */
static bool
text_format_parse_digits(const char **ptr,
						 const char *end_ptr,
						 int *value,
						 int location,
						 check_funcexpr_walker_params *wp,
						 bool *is_error)
{
	bool		found = false;
	const char *cp = *ptr;
	int			val = 0;

	*is_error = false;

	while (*cp >= '0' && *cp <= '9')
	{
		int			newval = val * 10 + (*cp - '0');

		if (newval / 10 != val) /* overflow? */
		{
			if (wp)
				plpgsql_check_put_error(wp->cstate,
										ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, 0,
										"number is out of range",
										NULL,
										NULL,
										PLPGSQL_CHECK_ERROR,
										location,
										wp->query_str, NULL);
			*is_error = true;
			return false;
		}
		val = newval;

		ADVANCE_PARSE_POINTER(cp, end_ptr);
		if (*is_error)
			return false;

		found = true;
	}

	*ptr = cp;
	*value = val;

	return found;
}

/*
 * reduced code from varlena.c
 */
static const char *
text_format_parse_format(const char *start_ptr,
						 const char *end_ptr,
						 int *argpos,
						 int *widthpos,
						 int location,
						 check_funcexpr_walker_params *wp,
						 bool *is_error)
{
	const char *cp = start_ptr;
	int			n;
	bool		found;

	/* set defaults for output parameters */
	*argpos = -1;
	*widthpos = -1;
	*is_error = false;

	/* try to identify first number */
	found = text_format_parse_digits(&cp, end_ptr, &n, location, wp, is_error);
	if (*is_error)
		return NULL;

	if (found)
	{
		if (*cp != '$')
		{
			/* Must be just a width and a type, so we're done */
			return cp;
		}
		/* The number was argument position */
		*argpos = n;
		/* Explicit 0 for argument index is immediately refused */
		if (n == 0)
		{
			if (wp)
				plpgsql_check_put_error(wp->cstate,
										ERRCODE_INVALID_PARAMETER_VALUE, 0,
										"format specifies argument 0, but arguments are numbered from 1",
										NULL,
										NULL,
										PLPGSQL_CHECK_ERROR,
										location,
										wp->query_str, NULL);
			*is_error = true;
			return NULL;
		}

		ADVANCE_PARSE_POINTER(cp, end_ptr);
		if (*is_error)
			return NULL;
	}

	/* Handle flags (only minus is supported now) */
	while (*cp == '-')
	{
		ADVANCE_PARSE_POINTER(cp, end_ptr);
		if (*is_error)
			return NULL;
	}

	if (*cp == '*')
	{
		/* Handle indirect width */
		ADVANCE_PARSE_POINTER(cp, end_ptr);
		if (*is_error)
			return NULL;

		found = text_format_parse_digits(&cp, end_ptr, &n, location, wp, is_error);
		if (*is_error)
			return NULL;

		if (found)
		{
			/* number in this position must be closed by $ */
			if (*cp != '$')
			{
				if (wp)
					plpgsql_check_put_error(wp->cstate,
											ERRCODE_INVALID_PARAMETER_VALUE, 0,
											"width argument position must be ended by \"$\"",
											NULL,
											NULL,
											PLPGSQL_CHECK_ERROR,
											location,
											wp->query_str, NULL);
				*is_error = true;
				return NULL;
			}

			/* The number was width argument position */
			*widthpos = n;
			/* Explicit 0 for argument index is immediately refused */
			if (n == 0)
			{
				if (wp)
					plpgsql_check_put_error(wp->cstate,
											ERRCODE_INVALID_PARAMETER_VALUE, 0,
											"format specifies argument 0, but arguments are numbered from 1",
											NULL,
											NULL,
											PLPGSQL_CHECK_ERROR,
											location,
											wp->query_str, NULL);
				*is_error = true;
				return NULL;
			}

			ADVANCE_PARSE_POINTER(cp, end_ptr);
			if (*is_error)
				return NULL;
		}
		else
			*widthpos = 0;		/* width's argument position is unspecified */
	}
	else
	{
		/* Check for direct width specification */
		found = text_format_parse_digits(&cp, end_ptr, &n, location, wp, is_error);
		if (*is_error)
			return NULL;
	}

	/* cp should now be pointing at type character */
	return cp;
}

#define TOO_FEW_ARGUMENTS_CHECK(arg, nargs) \
	if (arg > nargs) \
	{ \
		if (wp) \
			plpgsql_check_put_error(wp->cstate, \
									ERRCODE_INVALID_PARAMETER_VALUE, 0, \
									"too few arguments for format()", \
									NULL, \
									NULL, \
									PLPGSQL_CHECK_ERROR, \
									location, \
									wp->query_str, NULL); \
		*is_error = true; \
		return -1; \
	}

/*
 * Returns number of rquired arguments or -1 when we cannot detect this number
 */
static int
check_fmt_string(const char *fmt,
				 List *args,
				 int location,
				 check_funcexpr_walker_params *wp,
				 bool *is_error,
				 int *unsafe_expr_location)
{
	const char	   *cp;
	const char	   *end_ptr = fmt + strlen(fmt);
	int			nargs = list_length(args);
	int			required_nargs = 0;
	int			arg = 1;

	/* Scan format string, looking for conversion specifiers. */
	for (cp = fmt; cp < end_ptr; cp++)
	{
		int			argpos;
		int			widthpos;

		if (*cp != '%')
			continue;

		ADVANCE_PARSE_POINTER(cp, end_ptr);

		if (*cp == '%')
			continue;

		/* Parse the optional portions of the format specifier */
		cp = text_format_parse_format(cp, end_ptr,
									  &argpos, &widthpos,
									  location, wp, is_error);

		if (*is_error)
			return -1;

		/*
		 * Next we should see the main conversion specifier.  Whether or not
		 * an argument position was present, it's known that at least one
		 * character remains in the string at this point.  Experience suggests
		 * that it's worth checking that that character is one of the expected
		 * ones before we try to fetch arguments, so as to produce the least
		 * confusing response to a mis-formatted specifier.
		 */
		if (strchr("sIL", *cp) == NULL)
		{
			StringInfoData	sinfo;

			initStringInfo(&sinfo);

			appendStringInfo(&sinfo,
					"unrecognized format() type specifier \"%c\"", *cp);

			if (wp)
				plpgsql_check_put_error(wp->cstate,
										ERRCODE_INVALID_PARAMETER_VALUE, 0,
										sinfo.data,
										NULL,
										NULL,
										PLPGSQL_CHECK_ERROR,
										location,
										wp->query_str, NULL);

			pfree(sinfo.data);

			*is_error = true;
			return -1;
		}

		if (widthpos >= 0)
		{
			if (widthpos > 0)
			{
				TOO_FEW_ARGUMENTS_CHECK(widthpos, nargs);
				required_nargs = -1;
			}
			else
			{
				TOO_FEW_ARGUMENTS_CHECK(++arg, nargs);
				if (required_nargs != -1)
					required_nargs += 1;
			}
		}

		/* Check safety of argument againt SQL injection when it is required */
		if (unsafe_expr_location)
		{
			if (*cp == 's')
			{
				int		argn = argpos > 1 ? argpos : arg + 1;

				/* this is usually called after format check, but better be safe*/
				if (argn <= nargs)
				{
					Node *arg = list_nth(args, argn - 1);

					if (plpgsql_check_is_sql_injection_vulnerable(arg, unsafe_expr_location))
					{
						/* found vulnerability, stop */
						*is_error = false;
						return -1;
					}
				}
			}
		}

		if (argpos > 1)
		{
			TOO_FEW_ARGUMENTS_CHECK(argpos, nargs);
			required_nargs = -1;
		}
		else
		{
			TOO_FEW_ARGUMENTS_CHECK(++arg, nargs);
			if (required_nargs != -1)
				required_nargs += 1;
		}
	}

	return required_nargs;
}

/*
 * Try to detect relations in query
 */
static bool
has_rtable_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		bool		has_relation = false;
		ListCell *lc;

		foreach (lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			if (rte->rtekind == RTE_RELATION)
			{
				has_relation = true;
				break;
			}
		}

		if (has_relation)
		{
			return true;
		}
		else
			return query_tree_walker(query, has_rtable_walker, context, 0);
	}
	return expression_tree_walker(node, has_rtable_walker, context);
}

/*
 * Returns true, if query use any relation
 */
bool
plpgsql_check_has_rtable(Query *query)
{
	return has_rtable_walker((Node *) query, NULL);
}

/*
 * Try to identify constraint where variable from one side is implicitly casted to
 * parameter type of second side. It can symptom of parameter wrong type.
 *
 */
static bool
contain_fishy_cast_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *) node;

		if (!opexpr->opretset && opexpr->opresulttype == BOOLOID
				&& list_length(opexpr->args) == 2)
		{
			Node *l1 = linitial(opexpr->args);
			Node *l2 = lsecond(opexpr->args);
			Param *param = NULL;
			FuncExpr *fexpr = NULL;

			if (IsA(l1, Param))
				param = (Param *) l1;
			else if (IsA(l1, FuncExpr))
				fexpr = (FuncExpr *) l1;

			if (IsA(l2, Param))
				param = (Param *) l2;
			else if (IsA(l2, FuncExpr))
				fexpr = (FuncExpr *) l2;

			if (param != NULL && fexpr != NULL)
			{
				if (param->paramkind != PARAM_EXTERN)
					return false;

				if (fexpr->funcformat != COERCE_IMPLICIT_CAST ||
						fexpr->funcretset ||
						list_length(fexpr->args) != 1 ||
						param->paramtype != fexpr->funcresulttype)
					return false;

				if (!IsA(linitial(fexpr->args), Var))
					return false;

				*((Param **) context) = param;

				return true;
			}
		}
	}

	return expression_tree_walker(node, contain_fishy_cast_walker, context);
}

bool
plpgsql_check_qual_has_fishy_cast(PlannedStmt *plannedstmt, Plan *plan, Param **param)
{
	ListCell *lc;

	if (plan == NULL)
		return false;

	if (contain_fishy_cast_walker((Node *) plan->qual, param))
		return true;

	if (plpgsql_check_qual_has_fishy_cast(plannedstmt, innerPlan(plan), param))
		return true;
	if (plpgsql_check_qual_has_fishy_cast(plannedstmt, outerPlan(plan), param))
		return true;

	foreach(lc, plan->initPlan)
	{
		SubPlan *subplan = (SubPlan *) lfirst(lc);
		Plan *s_plan = exec_subplan_get_plan(plannedstmt, subplan);

		if (plpgsql_check_qual_has_fishy_cast(plannedstmt, s_plan, param))
			return true;
	}

	return false;
}

#define QUOTE_IDENT_OID			1282
#define QUOTE_LITERAL_OID		1283
#define QUOTE_NULLABLE_OID		1289

/*
 * Recursive iterate to deep and search extern params with typcategory "S", and check
 * if this value is sanitized.
 */
bool
plpgsql_check_is_sql_injection_vulnerable(Node *node, int *location)
{
	if (IsA(node, FuncExpr))
	{
		FuncExpr *fexpr = (FuncExpr *) node;
		bool	is_vulnerable = false;
		ListCell *lc;

		foreach(lc, fexpr->args)
		{
			Node *arg = lfirst(lc);

			if (plpgsql_check_is_sql_injection_vulnerable(arg, location))
			{
				is_vulnerable = true;
				break;
			}
		}

		if (is_vulnerable)
		{
			bool	typispreferred;
			char 	typcategory;

			get_type_category_preferred(fexpr->funcresulttype,
										&typcategory,
										&typispreferred);

			if (typcategory == 'S')
			{
				switch (fexpr->funcid)
				{
					case QUOTE_IDENT_OID:
					case QUOTE_LITERAL_OID:
					case QUOTE_NULLABLE_OID:
						return false;

					case FORMAT_0PARAM_OID:
					case FORMAT_NPARAM_OID:
						{
							/* We can do check only when first argument is constant */
							Node *first_arg = linitial(fexpr->args);

							if (first_arg && IsA(first_arg, Const))
							{
								Const *c = (Const *) first_arg;

								if (c->consttype == TEXTOID && !c->constisnull)
								{
									char *fmt = TextDatumGetCString(c->constvalue);
									bool	is_error;

									*location = -1;
									check_fmt_string(fmt, fexpr->args, c->location, NULL, &is_error, location);

									/* only in this case, "format" function obviously sanitize parameters */
									if (!is_error)
										return *location != -1;
								}
							}
						}
						break;
					default:
						/* do nothing */
							;
				}

				return true;
			}
		}

		return false;
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr *op = (OpExpr *) node;
		bool	is_vulnerable = false;
		ListCell *lc;

		foreach(lc, op->args)
		{
			Node *arg = lfirst(lc);

			if (plpgsql_check_is_sql_injection_vulnerable(arg, location))
			{
				is_vulnerable = true;
				break;
			}
		}

		if (is_vulnerable)
		{
			bool	typispreferred;
			char 	typcategory;

			get_type_category_preferred(op->opresulttype,
										&typcategory,
										&typispreferred);
			if (typcategory == 'S')
			{
				char *opname = get_opname(op->opno);
				bool	result = false;

				if (opname)
				{
					result = strcmp(opname, "||") == 0;

					pfree(opname);
				}

				return result;
			}
		}

		return false;
	}
	else if (IsA(node, NamedArgExpr))
	{
		return plpgsql_check_is_sql_injection_vulnerable((Node *) ((NamedArgExpr *) node)->arg, location);
	}
	else if (IsA(node, RelabelType))
	{
		return plpgsql_check_is_sql_injection_vulnerable((Node *) ((RelabelType *) node)->arg, location);
	}
	else if (IsA(node, Param))
	{
		Param *p = (Param *) node;

		if (p->paramkind == PARAM_EXTERN)
		{
			bool	typispreferred;
			char 	typcategory;

			get_type_category_preferred(p->paramtype, &typcategory, &typispreferred);
			if (typcategory == 'S')
			{
				*location = p->location;
				return true;
			}
		}

		return false;
	}
	else
		return false;
}
