/*
 *  TurboXSL XML+XSLT processing library
 *  Template precompiler / match routines
 *
 *
 *  (c) Egor Voznessenski, voznyak@mail.ru
 *
 *  $Id$
 *
**/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ltr_xsl.h"
#include "xsl_elements.h"

static int templtab_add(TRANSFORM_CONTEXT *pctx, XMLNODE * content, char *name)
{
  if(pctx->templcnt>=pctx->templno) 
  {
    pctx->templno += 100;
    pctx->templtab = realloc(pctx->templtab, pctx->templno*sizeof(TEMPLATE));
  }

  unsigned r = pctx->templcnt++;
  pctx->templtab[r].name = xmls_new_string_literal(name);
  pctx->templtab[r].content = content;
  pctx->templtab[r].match = NULL;
  pctx->templtab[r].mode = NULL;
  pctx->templtab[r].expr = NULL;
  pctx->templtab[r].priority = 0.0;
  pctx->templtab[r].matchtype = TMATCH_NONE;
  return r;
}

static unsigned add_templ_match(TRANSFORM_CONTEXT *pctx, XMLNODE *content, char *match, XMLSTRING mode)
{
  // skip leading whitespace;
  while(x_is_ws(*match)) ++match;

  //trailing whitespace
  for(size_t r=strlen(match);r;--r) {
    if(x_is_ws(match[r-1])) {
      match[r-1]=0;
    } 
    else {
      break;
    }
  }

  // if template match is empty, return empty node on match, not NULL
  if(!content) content = xml_new_node(pctx, NULL, NULL);

  XMLSTRING match_string = xmls_new_string_literal(match);

  for (unsigned i = 0; i < pctx->templcnt; i++) {
    if (xmls_equals(pctx->templtab[i].match, match_string)
        && xmls_equals(pctx->templtab[i].mode, mode)) {
      debug("add_templ_match:: found existing template: %d", i);
      pctx->templtab[i].content = content;
      return i;
    }
  }

  unsigned r = templtab_add(pctx, content, NULL);
  pctx->templtab[r].match = match_string;
  pctx->templtab[r].mode = mode;
  if(match[0] == '/' && match[1] == 0) {
    pctx->templtab[r].matchtype = TMATCH_ROOT;
  } else if(match[0] == '*' && match[1] == 0) {
    pctx->templtab[r].matchtype = TMATCH_ALWAYS;
  } else {
    pctx->templtab[r].matchtype = TMATCH_SELECT;
    pctx->templtab[r].expr = xpath_find_expr(pctx, match_string);
  }
  
  return r;
}

static void add_template(TRANSFORM_CONTEXT *pctx, XMLNODE *template, XMLSTRING name, XMLSTRING match, XMLSTRING mode)
{
  if(match) 
  {
    char *match_copy = xml_strdup(match->s);
    XMLNODE *content = template->children;
    while(match_copy)
    {
      if(strchr(match_copy,'['))
      {
        add_templ_match(pctx,content, match_copy, mode);
        match_copy = NULL;
      }
      else 
      {
        char *pp = strchr(match_copy,'|');
        if(pp) 
        {
          char *part_match = xml_new_string(match_copy, pp - match_copy);
          add_templ_match(pctx, content, part_match, mode);
          match_copy = pp + 1;
        }
        else 
        {
          add_templ_match(pctx,content, match_copy, mode);
          match_copy = NULL;
        }
      }
    }
  } 
  else 
  {
    if (dict_find(pctx->named_templ, name->s) != NULL)
    {
      debug("add_template:: replace template: %s", name->s);
      dict_replace(pctx->named_templ, name->s, template);
    }
    else
    {
      dict_add(pctx->named_templ, name->s, template);
    }
  }
}

static int select_match(TRANSFORM_CONTEXT *pctx, XMLNODE *node, XMLNODE *templ)
{
  XMLNODE *t;
  RVALUE rv,rv1;
  int retVal = 0;

  if (templ == NULL) // XXX TODO is this corrrect?
    return 1;

  if (node == NULL)
    return 0;

  switch(templ->type) 
  {
    case XPATH_NODE_ROOT:
      return (pctx->root_node == node);


    case XPATH_NODE_SELECT:
      if(!xmls_equals(node->name, templ->name))
        return 0;

      return select_match(pctx, node->parent, templ->children);


    case XPATH_NODE_CONTEXT:
        return 1;


    case XPATH_NODE_SELF:
        return select_match(pctx, node, templ->children);


    case XPATH_NODE_FILTER:
      if(!select_match(pctx, node, templ->children))
        return 0;

      return xpath_filter(pctx, NULL, node, templ->children->next) != NULL;


    case XPATH_NODE_UNION:
      for(;node;node=node->next) {
        for(t=templ->children;t;t=t->next) {
          if(select_match(pctx,node,t))
            return 1;
        }
      }
      return 0;


    case XPATH_NODE_ALL:
      return select_match(pctx, node->parent, templ->children);


    case XPATH_NODE_CALL:
      if(xmls_equals(templ->name, xsl_s_text))
        return node->type==TEXT_NODE;
      else if(xmls_equals(templ->name, xsl_s_node))
        return 1;

      return 0;


    case XPATH_NODE_EQ:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      xpath_execute_scalar(pctx, NULL, templ->children->next, node, &rv1);

      if (rval_equal(&rv, &rv1, 1))
        retVal = 1;

      rval_free(&rv);
      rval_free(&rv1);
      return retVal;


    case XPATH_NODE_NE:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      xpath_execute_scalar(pctx, NULL, templ->children->next, node, &rv1);

      if (rval_equal(&rv, &rv1, 0))
        retVal = 1;

      rval_free(&rv);
      rval_free(&rv1);
      return retVal;


    case XPATH_NODE_LE:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      xpath_execute_scalar(pctx, NULL, templ->children->next, node, &rv1);

      if (0 >= rval_compare(&rv, &rv1))
        retVal = 1;

      rval_free(&rv);
      rval_free(&rv1);
      return retVal;


    case XPATH_NODE_GE:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      xpath_execute_scalar(pctx, NULL, templ->children->next, node, &rv1);

      if (0 <= rval_compare(&rv, &rv1))
        retVal = 1;

      rval_free(&rv);
      rval_free(&rv1);
      return retVal;


    case XPATH_NODE_LT:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      xpath_execute_scalar(pctx, NULL, templ->children->next, node, &rv1);

      if (0 > rval_compare(&rv, &rv1))
        retVal = 1;

      rval_free(&rv);
      rval_free(&rv1);
      return retVal;


    case XPATH_NODE_GT:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      xpath_execute_scalar(pctx, NULL, templ->children->next, node, &rv1);

      if (0 < rval_compare(&rv, &rv1))
        retVal = 1;

      rval_free(&rv);
      rval_free(&rv1);
      return retVal;

    case XPATH_NODE_NOT:
      xpath_execute_scalar(pctx, NULL, templ->children, node, &rv);
      retVal = rval2bool(&rv) ? 0 : 1;
      rval_free(&rv);
      return retVal;


    case XPATH_NODE_OR:
      for(t=templ->children;t;t=t->next) {
        if(select_match(pctx,node,t))
          return 1;
      }

      return 0;


    case XPATH_NODE_AND:
      for(t=templ->children;t;t=t->next) {
        if(!select_match(pctx,node,t))
          return 0;
      }

      return 1;


    default:
      return 0;
  }
}

XMLNODE *find_template(TRANSFORM_CONTEXT *pctx, XMLNODE *node, XMLSTRING mode)
{
  XMLNODE *template = NULL;
  unsigned int expression_length = 0;
  for(int i=0;i<pctx->templcnt;++i) {
    if(!xmls_equals(pctx->templtab[i].mode, mode)) continue;

    if(pctx->templtab[i].matchtype == TMATCH_ROOT) {
      if(node == pctx->root_node) return pctx->templtab[i].content;
    }

    if(pctx->templtab[i].matchtype == TMATCH_SELECT) {
      if(select_match(pctx, node, pctx->templtab[i].expr)) {
        XMLNODE *command = pctx->templtab[i].expr;
        unsigned int length = 0;
        while(command != NULL) {
          length = length + 1;
          command = command->children;
        }
        if(length > expression_length) {
          expression_length = length;
          template = pctx->templtab[i].content;
        }
      }
    }
  }

  if (template != NULL) return template;

  if(node!=pctx->root_node) {
    for(int i=0;i<pctx->templcnt;++i) {
      if(xmls_equals(pctx->templtab[i].mode, mode) && pctx->templtab[i].matchtype == TMATCH_ALWAYS) {
        return pctx->templtab[i].content;
      }
    }
  }

  return NULL;
}

XMLNODE *template_byname(TRANSFORM_CONTEXT *pctx, XMLSTRING name)
{
  if(name==NULL)
    return NULL;

  XMLNODE *template = dict_find(pctx->named_templ,name->s);
  return template == NULL ? NULL : template->children;
}

void precompile_templates(TRANSFORM_CONTEXT *pctx, XMLNODE *node)
{
  for(;node;node=node->next) 
  {
    if(node->type==ELEMENT_NODE && xmls_equals(node->name, xsl_template))
    {
      XMLSTRING name  = xml_get_attr(node,xsl_a_name);
      XMLSTRING match = xml_get_attr(node,xsl_a_match);
      XMLSTRING mode  = xml_get_attr(node,xsl_a_mode);
      add_template(pctx, node, name, match, mode);
    }

    if(node->children)
    {
      precompile_templates(pctx, node->children);
    }
  }
}
