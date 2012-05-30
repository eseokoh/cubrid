/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */


/*
 * shard_statement.c -
 */

#ident "$Id$"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "cas_common.h"
#include "shard_proxy.h"
#include "shard_proxy_common.h"
#include "shard_statement.h"
#include "shard_shm.h"

extern T_SHM_SHARD_KEY *shm_key_p;
extern T_PROXY_INFO *proxy_info_p;

static int shard_stmt_lru_insert (T_SHARD_STMT * stmt_p);
static int shard_stmt_lru_delete (T_SHARD_STMT * stmt_p);
static T_SHARD_STMT *shard_stmt_get_lru (void);

static int *shard_stmt_pos_srv_h_id (T_SHARD_STMT * stmt_p, int shard_id,
				     int cas_id);
static int shard_stmt_find_srv_h_id (T_SHARD_STMT * stmt_p, int shard_id,
				     int cas_id);
static int shard_stmt_set_srv_h_id (T_SHARD_STMT * stmt_p, int shard_id,
				    int cas_id, int srv_h_id);

static T_SHARD_STMT *shard_stmt_find_unused (void);
static int shard_stmt_change_shard_val_to_id (char **sql_stmt,
					      const char **buf,
					      char appl_server);
static char *shard_stmt_write_buf_to_sql (char *sql_stmt, const char *buf,
					  int length, bool is_to_upper,
					  char appl_server);

T_SHARD_STMT_GLOBAL shard_Stmt;

int shard_Stmt_max_num_alloc = SHARD_STMT_MAX_NUM_ALLOC;

static int
shard_stmt_lru_insert (T_SHARD_STMT * stmt_p)
{
  if ((stmt_p->num_pinned != 0)
      || (stmt_p->lru_prev != NULL) || (stmt_p->lru_next != NULL))
    {
      PROXY_DEBUG_LOG ("Invalid statement status. statement(%s).",
		       shard_str_stmt (stmt_p));

      assert (false);
      return -1;
    }

  stmt_p->lru_next = NULL;
  stmt_p->lru_prev = shard_Stmt.mru;

  if (shard_Stmt.mru)
    {
      shard_Stmt.mru->lru_next = stmt_p;
    }
  else
    {
      shard_Stmt.lru = stmt_p;
    }

  shard_Stmt.mru = stmt_p;

  return 0;
}

static int
shard_stmt_lru_delete (T_SHARD_STMT * stmt_p)
{
  ENTER_FUNC ();

  if ((stmt_p->lru_prev == NULL && shard_Stmt.lru != stmt_p)
      || (stmt_p->lru_next == NULL && shard_Stmt.mru != stmt_p))
    {
      PROXY_DEBUG_LOG
	("Invalid statement lru list. "
	 "(stmt_p:%p, lru:%p, mru:%p). "
	 "statement(%s).",
	 stmt_p, shard_Stmt.lru, shard_Stmt.mru, shard_str_stmt (stmt_p));

      assert (false);

      EXIT_FUNC ();
      return -1;
    }

  if (stmt_p->lru_next)
    {
      stmt_p->lru_next->lru_prev = stmt_p->lru_prev;
    }
  else
    {
      shard_Stmt.mru = stmt_p->lru_prev;
    }

  if (stmt_p->lru_prev)
    {
      stmt_p->lru_prev->lru_next = stmt_p->lru_next;
    }
  else
    {
      shard_Stmt.lru = stmt_p->lru_next;
    }

  stmt_p->lru_prev = NULL;
  stmt_p->lru_next = NULL;

  EXIT_FUNC ();
  return 0;
}

static T_SHARD_STMT *
shard_stmt_get_lru (void)
{
  return shard_Stmt.lru;
}

static int *
shard_stmt_pos_srv_h_id (T_SHARD_STMT * stmt_p, int shard_id, int cas_id)
{
  int pos;
  int srv_h_id;

  if ((shard_id < 0 || shard_Stmt.max_num_shard <= shard_id)
      || (cas_id < 0 || shard_Stmt.num_cas_per_shard <= cas_id))
    {
      PROXY_DEBUG_LOG ("Invalid statement shard/CAS id. "
		       "(shard_id:%d, cas_id:%d, max_num_shard:%d, "
		       "num_cas_per_shard:%d). statement(%s).",
		       shard_id, cas_id, shard_Stmt.max_num_shard,
		       shard_Stmt.num_cas_per_shard, shard_str_stmt (stmt_p));

      assert (false);
      return NULL;
    }

  pos = (shard_id * shard_Stmt.num_cas_per_shard) + cas_id;
  srv_h_id = stmt_p->srv_h_id_ent[pos];

  return &(stmt_p->srv_h_id_ent[pos]);
}

static int
shard_stmt_find_srv_h_id (T_SHARD_STMT * stmt_p, int shard_id, int cas_id)
{
  int *srv_h_id_p;

  srv_h_id_p = shard_stmt_pos_srv_h_id (stmt_p, shard_id, cas_id);
  if (srv_h_id_p == NULL)
    {
      return -1;
    }

  return (*srv_h_id_p);
}

static int
shard_stmt_set_srv_h_id (T_SHARD_STMT * stmt_p, int shard_id, int cas_id,
			 int srv_h_id)
{
  int *srv_h_id_p;

  srv_h_id_p = shard_stmt_pos_srv_h_id (stmt_p, shard_id, cas_id);
  if (srv_h_id_p == NULL)
    {
      return -1;
    }

  *srv_h_id_p = srv_h_id;
  return 0;
}

T_SHARD_STMT *
shard_stmt_find_by_sql (char *sql_stmt)
{
  T_SHARD_STMT *stmt_p = NULL;
  int i;

  for (i = 0; i < shard_Stmt.max_num_stmt; i++)
    {
      stmt_p = &(shard_Stmt.stmt_ent[i]);
      if (stmt_p->status == SHARD_STMT_STATUS_UNUSED
	  || strcmp (sp_get_sql_stmt (stmt_p->parser), sql_stmt))
	{
	  continue;
	}

      return stmt_p;
    }

  return NULL;
}

T_SHARD_STMT *
shard_stmt_find_by_stmt_h_id (int stmt_h_id)
{
  T_SHARD_STMT *stmt_p = NULL;
  unsigned hash_res;

  hash_res = (stmt_h_id % shard_Stmt.max_num_stmt);

  stmt_p = &(shard_Stmt.stmt_ent[hash_res]);
  if (stmt_p->status != SHARD_STMT_STATUS_UNUSED
      && stmt_p->stmt_h_id == stmt_h_id)
    {
      return stmt_p;
    }

  return NULL;
}

static T_SHARD_STMT *
shard_stmt_find_unused (void)
{
  T_SHARD_STMT *stmt_p = NULL;
  int i;

  for (i = 0; i < shard_Stmt.max_num_stmt; i++)
    {
      stmt_p = &(shard_Stmt.stmt_ent[i]);
      if (stmt_p->status != SHARD_STMT_STATUS_UNUSED)
	{
	  continue;
	}

      return stmt_p;
    }

  return NULL;
}

int
shard_stmt_pin (T_SHARD_STMT * stmt_p)
{
  int error = 0;

  if (stmt_p->status == SHARD_STMT_STATUS_UNUSED)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Invalid statement status. statement(%s).",
		 shard_str_stmt (stmt_p));
      assert (false);
      return -1;
    }

  stmt_p->num_pinned += 1;

  if ((shard_Stmt.lru == stmt_p) || (stmt_p->lru_prev || stmt_p->lru_next))
    {
      PROXY_DEBUG_LOG ("Pin statement. statement(%s).",
		       shard_str_stmt (stmt_p));

      error = shard_stmt_lru_delete (stmt_p);
    }
  return error;
}

int
shard_stmt_unpin (T_SHARD_STMT * stmt_p)
{
  int error = 0;

  if (stmt_p->status == SHARD_STMT_STATUS_UNUSED)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Invalid statement status. statement(%s).",
		 shard_str_stmt (stmt_p));
      assert (false);
      return -1;
    }

  stmt_p->num_pinned -= 1;
  assert (stmt_p->num_pinned >= 0);

  if (stmt_p->num_pinned <= 0)
    {
      stmt_p->num_pinned = 0;

      error = shard_stmt_lru_insert (stmt_p);
    }

  return error;
}

void
shard_stmt_check_waiter_and_wakeup (T_SHARD_STMT * stmt_p)
{
  int error;
  T_WAIT_CONTEXT *waiter_p = NULL;

  ENTER_FUNC ();

  assert (stmt_p);

  /* clear context owner */
  stmt_p->ctx_cid = PROXY_INVALID_CONTEXT;
  stmt_p->ctx_uid = 0;

  while ((waiter_p =
	  (T_WAIT_CONTEXT *) shard_queue_dequeue (&stmt_p->waitq)) != NULL)
    {
      PROXY_DEBUG_LOG ("Wakeup context by statement. statement(%s).",
		       shard_str_stmt (stmt_p));

      error = proxy_wakeup_context_by_statement (waiter_p);
      if (error)
	{
	  PROXY_LOG (PROXY_LOG_MODE_ERROR,
		     "Failed to wakeup context by statement. "
		     "(error:%d, context id:%d, context uid:%d). statement(%s).",
		     error, waiter_p->ctx_cid, waiter_p->ctx_uid,
		     shard_str_stmt (stmt_p));

	  assert (false);
	}

      FREE_MEM (waiter_p);
    }

  EXIT_FUNC ();
  return;
}

T_SHARD_STMT *
shard_stmt_new (char *sql_stmt, int ctx_cid, unsigned int ctx_uid)
{
  int error;
  int i, num_cas;
  char *sql_stmt_tp = NULL;
  T_SHARD_STMT *stmt_p = NULL;

  assert (sql_stmt);

  stmt_p = shard_stmt_find_by_sql (sql_stmt);
  if (stmt_p)
    {
      return stmt_p;
    }

  num_cas = shard_Stmt.max_num_shard * shard_Stmt.num_cas_per_shard;

  stmt_p = shard_stmt_find_unused ();
  if (stmt_p)
    {
      assert (stmt_p->num_pinned == 0);
      assert (stmt_p->lru_next == NULL && stmt_p->lru_prev == NULL);

      if (!(stmt_p->num_pinned == 0)
	  || !(stmt_p->lru_next == NULL && stmt_p->lru_prev == NULL))
	{
	  return NULL;
	}
    }
  else
    {
      stmt_p = shard_stmt_get_lru ();
      if (stmt_p)
	{
	  shard_stmt_free (stmt_p);
	}
      else
	{
	  PROXY_DEBUG_LOG ("Exceeds max prepared statement. ");
	  return NULL;
	}
    }

  if (stmt_p)
    {
      if ((int) stmt_p->num_alloc >= shard_Stmt_max_num_alloc)
	{
	  stmt_p->num_alloc = 0;
	}
      stmt_p->stmt_h_id =
	(stmt_p->num_alloc * shard_Stmt.max_num_stmt) + stmt_p->index;

      stmt_p->status = SHARD_STMT_STATUS_IN_PROGRESS;

      stmt_p->ctx_cid = ctx_cid;
      stmt_p->ctx_uid = ctx_uid;

      stmt_p->num_pinned = 0;
      stmt_p->lru_prev = NULL;
      stmt_p->lru_next = NULL;

      stmt_p->parser = sp_create_parser (sql_stmt);
      if (stmt_p->parser == NULL)
	{
	  shard_stmt_free (stmt_p);
	  return NULL;
	}

      /* __FOR_DEBUG */
      assert (stmt_p->request_buffer_length == 0);
      assert (stmt_p->request_buffer == NULL);
      assert (stmt_p->reply_buffer_length == 0);
      assert (stmt_p->reply_buffer == NULL);

      stmt_p->request_buffer_length = 0;
      stmt_p->request_buffer = NULL;
      stmt_p->reply_buffer_length = 0;
      stmt_p->reply_buffer = NULL;

      for (i = 0; i < num_cas; i++)
	{
	  stmt_p->srv_h_id_ent[i] = SHARD_STMT_INVALID_HANDLE_ID;
	}

      error = shard_queue_initialize (&stmt_p->waitq);
      if (error)
	{
	  assert (false);

	  shard_stmt_free (stmt_p);
	  return NULL;
	}

      stmt_p->num_alloc += 1;
    }
  return stmt_p;
}

void
shard_stmt_destroy (void)
{
  T_SHARD_STMT *stmt_p = NULL;
  int i;

  if (shard_Stmt.stmt_ent)
    {
      for (i = 0; i < shard_Stmt.max_num_stmt; i++)
	{
	  stmt_p = &(shard_Stmt.stmt_ent[i]);

	  stmt_p->stmt_h_id = SHARD_STMT_INVALID_HANDLE_ID;
	  stmt_p->status = SHARD_STMT_STATUS_UNUSED;

	  stmt_p->ctx_cid = PROXY_INVALID_CONTEXT;
	  stmt_p->ctx_uid = 0;

	  stmt_p->num_pinned = 0;
	  if ((shard_Stmt.lru == stmt_p)
	      || (stmt_p->lru_prev || stmt_p->lru_next))
	    {
	      PROXY_DEBUG_LOG ("Delete statement from lru. "
			       "statement(%p, %s). ",
			       stmt_p, shard_str_stmt (stmt_p));

	      shard_stmt_lru_delete (stmt_p);
	    }
	  stmt_p->lru_prev = NULL;
	  stmt_p->lru_next = NULL;

	  if (stmt_p->parser)
	    {
	      sp_destroy_parser (stmt_p->parser);
	    }
	  stmt_p->parser = NULL;

	  FREE_MEM (stmt_p->request_buffer);
	  stmt_p->request_buffer_length = 0;

	  FREE_MEM (stmt_p->reply_buffer);
	  stmt_p->reply_buffer_length = 0;


	  FREE_MEM (stmt_p->srv_h_id_ent);

	  shard_queue_destroy (&stmt_p->waitq);
	}

      FREE_MEM (shard_Stmt.stmt_ent);
    }

  shard_Stmt.max_num_stmt = 0;
  shard_Stmt.max_num_shard = 0;
  shard_Stmt.num_cas_per_shard = 0;

  return;
}

void
shard_stmt_free (T_SHARD_STMT * stmt_p)
{
  int error;
  int num_cas;
  int i;

  ENTER_FUNC ();

  assert (stmt_p);

  num_cas = shard_Stmt.max_num_shard * shard_Stmt.num_cas_per_shard;

  stmt_p->stmt_h_id = SHARD_STMT_INVALID_HANDLE_ID;
  stmt_p->status = SHARD_STMT_STATUS_UNUSED;

  stmt_p->ctx_cid = PROXY_INVALID_CONTEXT;
  stmt_p->ctx_uid = 0;

  stmt_p->num_pinned = 0;
  if ((shard_Stmt.lru == stmt_p) || (stmt_p->lru_prev || stmt_p->lru_next))
    {
      PROXY_DEBUG_LOG ("Delete statement from lru. "
		       "statement(%p, %s).", stmt_p, shard_str_stmt (stmt_p));

      shard_stmt_lru_delete (stmt_p);
    }
  stmt_p->lru_prev = NULL;
  stmt_p->lru_next = NULL;

  if (stmt_p->parser)
    {
      sp_destroy_parser (stmt_p->parser);
    }
  stmt_p->parser = NULL;

  FREE_MEM (stmt_p->request_buffer);
  stmt_p->request_buffer_length = 0;

  FREE_MEM (stmt_p->reply_buffer);
  stmt_p->reply_buffer_length = 0;


  for (i = 0; i < num_cas; i++)
    {
      stmt_p->srv_h_id_ent[i] = SHARD_STMT_INVALID_HANDLE_ID;
    }

  shard_queue_destroy (&stmt_p->waitq);

  EXIT_FUNC ();
  return;
}


/* 
 * 
 * return : stored server handle id 
 */
int
shard_stmt_find_srv_h_id_for_shard_cas (int stmt_h_id, int shard_id,
					int cas_id)
{
  int srv_h_id;
  T_SHARD_STMT *stmt_p = NULL;

  stmt_p = shard_stmt_find_by_stmt_h_id (stmt_h_id);
  if (stmt_p == NULL)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Unable to find statement. (stmt_h_id:%d).", stmt_h_id);
      return -1;
    }

  srv_h_id = shard_stmt_find_srv_h_id (stmt_p, shard_id, cas_id);
  if (srv_h_id == SHARD_STMT_INVALID_HANDLE_ID)
    {
      PROXY_LOG (PROXY_LOG_MODE_SHARD_DETAIL,
		 "Unable to find saved statement handle id. "
		 "(shard_id:%d, cas_id:%d). stmt(%s).", shard_id, cas_id,
		 shard_str_stmt (stmt_p));
      return -1;
    }

  return srv_h_id;
}

/* 
 *
 * return : success or fail
 */
int
shard_stmt_add_srv_h_id_for_shard_cas (int stmt_h_id, int shard_id,
				       int cas_id, int srv_h_id)
{
  int error;
  int ref_count;

  T_SHARD_STMT *stmt_p = NULL;

  stmt_p = shard_stmt_find_by_stmt_h_id (stmt_h_id);
  if (stmt_p == NULL)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Unable to find statement. (stmt_h_id:%d).", stmt_h_id);
      return -1;
    }

  stmt_p->status = SHARD_STMT_STATUS_COMPLETE;

  /* check and wakeup statement waiter */
  shard_stmt_check_waiter_and_wakeup (stmt_p);

  PROXY_DEBUG_LOG ("Create new sql statement. "
		   "(shard_id:%d, cas_id:%d, srv_h_id:%d). statement(%s).",
		   shard_id, cas_id, srv_h_id, shard_str_stmt (stmt_p));

  error = shard_stmt_set_srv_h_id (stmt_p, shard_id, cas_id, srv_h_id);
  if (error)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Unable to save statement handle id. "
		 "(shard_id:%d, cas_id:%d, srv_h_id:%d). stmt(%s).",
		 shard_id, cas_id, srv_h_id, shard_str_stmt (stmt_p));
      return -1;
    }

  return 0;
}

void
shard_stmt_del_srv_h_id_for_shard_cas (int stmt_h_id, int shard_id,
				       int cas_id)
{
  int error;
  int ref_count;
  int *srv_h_id_p;
  T_SHARD_STMT *stmt_p = NULL;

  ENTER_FUNC ();

  stmt_p = shard_stmt_find_by_stmt_h_id (stmt_h_id);
  if (stmt_p == NULL)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Unable to find statement. (stmt_h_id:%d).", stmt_h_id);
      return;
    }

  srv_h_id_p = shard_stmt_pos_srv_h_id (stmt_p, shard_id, cas_id);
  if (srv_h_id_p)
    {
      *srv_h_id_p = SHARD_STMT_INVALID_HANDLE_ID;
    }

  EXIT_FUNC ();
  return;
}

void
shard_stmt_del_all_srv_h_id_for_shard_cas (int shard_id, int cas_id)
{
  int i;
  int ref_count;
  int *srv_h_id_p;

  ENTER_FUNC ();

  T_SHARD_STMT *stmt_p = NULL;

  for (i = 0; i < shard_Stmt.max_num_stmt; i++)
    {
      stmt_p = &(shard_Stmt.stmt_ent[i]);
      if (stmt_p->status == SHARD_STMT_STATUS_UNUSED)
	{
	  continue;
	}

      srv_h_id_p = shard_stmt_pos_srv_h_id (stmt_p, shard_id, cas_id);
      if (srv_h_id_p)
	{
	  *srv_h_id_p = SHARD_STMT_INVALID_HANDLE_ID;
	}
    }

  EXIT_FUNC ();
  return;
}

int
shard_stmt_set_hint_list (T_SHARD_STMT * stmt_p)
{
  int ret;
  SP_PARSER_HINT *hint_p = NULL;
  SP_HINT_TYPE hint_type = HT_NONE;

  ret = sp_parse_sql (stmt_p->parser);
  if (ret < 0)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR, "Failed to parse sql statement. "
		 "(error:%d). stmt(%s).", ret, shard_str_stmt (stmt_p));
      return -1;
    }

  hint_p = sp_get_first_hint (stmt_p->parser);
  while (hint_p != NULL)
    {
      if (hint_type == HT_NONE)
	{
	  hint_type = hint_p->hint_type;
	}
      else if (hint_type != hint_p->hint_type)
	{
	  PROXY_LOG (PROXY_LOG_MODE_ERROR,
		     "Unexpected hint type. (expect:%d, current:%d).",
		     hint_p->hint_type, hint_type);
	  return -1;
	}

      /* currently only support HT_KEY and HT_ID */
      if (hint_type != HT_KEY && hint_type != HT_ID)
	{
	  PROXY_LOG (PROXY_LOG_MODE_ERROR,
		     "Unsupported hint type. (hint_type:%d).", hint_type);
	  return -1;
	}
      hint_p = sp_get_next_hint (hint_p);
    }
  return 1;
}

int
shard_stmt_get_hint_type (T_SHARD_STMT * stmt_p)
{
  SP_PARSER_HINT *hint_p = NULL;

  if (stmt_p == NULL)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Invalid statement. Statement couldn't be NULL.");
      return HT_INVAL;
    }
  if (stmt_p->parser == NULL)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Invalid parser. Statement parser couldn't be NULL.");
      return HT_INVAL;
    }
  hint_p = sp_get_first_hint (stmt_p->parser);
  if (hint_p == NULL)
    {
      return HT_NONE;
    }

  return hint_p->hint_type;
}

#if defined (PROXY_VERBOSE_DEBUG)
void
shard_stmt_dump_title (FILE * fp)
{
  fprintf (fp, "[%-10s][%-5s] %-10s %-2s %-10s %-10s %-10s\n",
	   "INDEX", "NUSED",
	   "STMT_H_ID", "ST", "NUM_PINNED", "LRU_NEXT", "LRU_PREV");

  return;
}

void
shard_stmt_dump (FILE * fp, T_SHARD_STMT * stmt_p)
{
  fprintf (fp, "[%-10u][%-5u] %-10d %-2d %-10d %-10p %-10p\n",
	   stmt_p->index, stmt_p->num_alloc,
	   stmt_p->stmt_h_id, stmt_p->status,
	   stmt_p->num_pinned, stmt_p->lru_next, stmt_p->lru_prev);

  return;
}

void
shard_stmt_dump_all (FILE * fp)
{
  T_SHARD_STMT *stmt_p;
  int i;

  fprintf (fp, "\n");
  fprintf (fp, "* %-20s : %d\n", "MAX_NUM_STMT", shard_Stmt.max_num_stmt);
  fprintf (fp, "* %-20s : %d\n", "MAX_NUM_SHARD", shard_Stmt.max_num_shard);
  fprintf (fp, "* %-20s : %d\n", "NUM_CAS_PER_SHARD",
	   shard_Stmt.num_cas_per_shard);
  fprintf (fp, "\n");

  shard_stmt_dump_title (fp);
  for (i = 0; i < shard_Stmt.max_num_stmt; i++)
    {
      stmt_p = &(shard_Stmt.stmt_ent[i]);
      shard_stmt_dump (fp, stmt_p);
    }

  return;
}
#endif /* ENABLE_UNUSED_FUNCTION */

char *
shard_str_stmt (T_SHARD_STMT * stmt_p)
{
  static char buffer[LINE_MAX];

  if (stmt_p == NULL)
    {
      return (char *) "-";
    }

  snprintf (buffer, sizeof (buffer),
	    "index:%u, num_alloc:%u, "
	    "stmt_h_id:%d, status:%d, "
	    "context id:%d, context uid:%d, "
	    "num pinned:%d, "
	    "lru_next:%p, lru_prev:%p, "
	    "sql_stmt:[%s]",
	    stmt_p->index, stmt_p->num_alloc,
	    stmt_p->stmt_h_id, stmt_p->status,
	    stmt_p->ctx_cid, stmt_p->ctx_uid,
	    stmt_p->num_pinned,
	    stmt_p->lru_next, stmt_p->lru_prev,
	    (stmt_p->parser) ? ((stmt_p->parser->sql_stmt) ?
				stmt_p->parser->sql_stmt : "-") : "-");

  return (char *) buffer;
}

int
shard_stmt_initialize (int initial_size)
{
  int error;
  T_SHARD_STMT *stmt_p;
  T_SHARD_INFO *shard_info_p;
  int mem_size;
  int num_cas;
  int i, j;

  static bool is_init = false;

  if (is_init)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR,
		 "Statement have not been initialized.");
      return -1;
    }

  shard_info_p = shard_shm_get_first_shard_info (proxy_info_p);

  mem_size = initial_size * sizeof (T_SHARD_STMT);
  shard_Stmt.stmt_ent = (T_SHARD_STMT *) malloc (mem_size);
  if (shard_Stmt.stmt_ent == NULL)
    {
      PROXY_LOG (PROXY_LOG_MODE_ERROR, "Not enough virtual memory. "
		 "Failed to alloc statement entries. "
		 "(errno:%d, size:%d).", errno, mem_size);

      return -1;
    }
  memset (shard_Stmt.stmt_ent, 0, mem_size);

  shard_Stmt.max_num_stmt = initial_size;
  shard_Stmt.max_num_shard = proxy_info_p->max_shard;
  shard_Stmt.num_cas_per_shard = shard_info_p->max_appl_server;
  shard_Stmt.mru = NULL;
  shard_Stmt.lru = NULL;

  shard_Stmt_max_num_alloc =
    MAX (shard_Stmt_max_num_alloc, (INT_MAX / shard_Stmt.max_num_stmt) - 1);

  num_cas = shard_Stmt.max_num_shard * shard_Stmt.num_cas_per_shard;

  for (i = 0; i < shard_Stmt.max_num_stmt; i++)
    {
      stmt_p = &(shard_Stmt.stmt_ent[i]);
      memset (stmt_p, 0, sizeof (T_SHARD_STMT));

      stmt_p->index = i;
      stmt_p->ctx_cid = PROXY_INVALID_CONTEXT;
      stmt_p->ctx_uid = 0;

      stmt_p->srv_h_id_ent = (int *) malloc (num_cas * sizeof (int));
      if (stmt_p->srv_h_id_ent == NULL)
	{
	  PROXY_LOG (PROXY_LOG_MODE_ERROR, "Not enough virtual memory. "
		     "Failed to alloc statement handle id entries. "
		     "(errno:%d, size:%d).", errno, mem_size);


	  return -1;
	}
      for (j = 0; j < num_cas; j++)
	{
	  stmt_p->srv_h_id_ent[j] = SHARD_STMT_INVALID_HANDLE_ID;
	}
    }

  return 0;
}

char *
shard_stmt_rewrite_sql (char *sql_stmt, char appl_server)
{
  int error = NO_ERROR;
  const char *p = NULL;
  const char *next_p = NULL;
  char *q = NULL;
  char *organized_sql_stmt = NULL;
  SP_HINT_TYPE hint_type = HT_NONE;
#if 0				/* for fully rewriting sql statement */
  SP_TOKEN curr_token;
  SP_TOKEN prev_token;
  int whitespace_count = 0;
#endif

  while (*sql_stmt && isspace (*sql_stmt))
    {
      sql_stmt++;
    }

  /**
   * The 'shard_val(n)' is changed to 'shard_id(N)'.
   * a minimum number of chipers of n is 1;
   * a maximum number of chipers of N is 3;
   * so length of organized sql can be larger than length of original sql.
   */
  organized_sql_stmt =
    (char *) malloc (sizeof (char) * (strlen (sql_stmt) + 2));
  if (organized_sql_stmt == NULL)
    {
      return NULL;
    }

  p = sql_stmt;
  q = organized_sql_stmt;

#if 0				/* for fully rewriting sql statement */
  prev_token = curr_token = TT_NONE;
  while (*p)
    {
      next_p = sp_get_token_type (p, &curr_token);

      switch (prev_token)
	{
	case TT_SINGLE_QUOTED:
	case TT_DOUBLE_QUOTED:
	  switch (curr_token)
	    {
	    case TT_SINGLE_QUOTED:
	    case TT_DOUBLE_QUOTED:
	      q = shard_stmt_write_buf_to_sql (q, "'", 1, true, appl_server);
	      break;
	    default:
	      q =
		shard_stmt_write_buf_to_sql (q, p, next_p - p, false,
					     appl_server);
	    }
	  break;
	case TT_CPP_COMMENT:
	case TT_CSQL_COMMENT:
	  if (curr_token == TT_NEWLINE)
	    {
	      whitespace_count = 0;
	      q = shard_stmt_write_buf_to_sql (q, "*/", 2, true, appl_server);
	      break;
	    }
	default:
	  switch (curr_token)
	    {
	    case TT_NEWLINE:
	    case TT_WHITESPACE:
	      if (whitespace_count++ < 1)
		{
		  q =
		    shard_stmt_write_buf_to_sql (q, " ", 1, true,
						 appl_server);
		}
	      break;
	    case TT_SINGLE_QUOTED:
	    case TT_DOUBLE_QUOTED:
	      q = shard_stmt_write_buf_to_sql (q, "'", 1, true, appl_server);
	      break;
	    case TT_CSQL_COMMENT:
	    case TT_CPP_COMMENT:
	      q = shard_stmt_write_buf_to_sql (q, "/*", 2, true, appl_server);
	      p = next_p;
	      if (*p == '+')
		{
		  p++;
		  p = sp_get_hint_type (p, &hint_type);
		  if (hint_type == HT_VAL)
		    {
		      error =
			shard_stmt_change_shard_val_to_id (&q, &p,
							   appl_server);
		      if (error != NO_ERROR)
			{
			  free (organized_sql_stmt);
			  organized_sql_stmt = NULL;

			  return NULL;
			}
		      next_p = p;
		    }
		}
	      break;
	    case TT_HINT:
	      q =
		shard_stmt_write_buf_to_sql (q, "/*+", 3, true, appl_server);
	      p = next_p;
	      p = sp_get_hint_type (p, &hint_type);
	      if (hint_type == HT_VAL)
		{
		  error =
		    shard_stmt_change_shard_val_to_id (&q, &p, appl_server);
		  if (error != NO_ERROR)
		    {
		      free (organized_sql_stmt);
		      organized_sql_stmt = NULL;

		      return NULL;
		    }
		  next_p = p;
		}
	      break;
	    default:
	      q =
		shard_stmt_write_buf_to_sql (q, p, next_p - p, true,
					     appl_server);
	      break;
	    }
	}

      if (curr_token != TT_WHITESPACE && curr_token != TT_NEWLINE)
	{
	  whitespace_count = 0;
	}

      if (prev_token == TT_NONE)
	{
	  if (sp_is_exist_pair_token (curr_token))
	    {
	      prev_token = curr_token;
	    }
	}
      else if (sp_is_pair_token (prev_token, curr_token))
	{
	  prev_token = TT_NONE;
	}

      p = next_p;
    }
#else
  while (*(p))
    {
      if (*(p) == '/' && *(p + 1) == '*' && *(p + 2) == '+')
	{
	  q = shard_stmt_write_buf_to_sql (q, "/*+", 3, true, appl_server);
	  p = p + 3;
	  next_p = p;
	  p = sp_get_hint_type (p, &hint_type);
	  if (hint_type == HT_VAL)
	    {
	      error = shard_stmt_change_shard_val_to_id (&q, &p, appl_server);
	      if (error != NO_ERROR)
		{
		  free (organized_sql_stmt);
		  organized_sql_stmt = NULL;

		  return NULL;
		}
	    }
	  else
	    {
	      p = next_p;
	    }
	}
      else
	{
	  q = shard_stmt_write_buf_to_sql (q, p, 1, true, appl_server);
	  p += 1;
	}
    }
#endif

  *(q++) = '\0';		/* NTS */

  return organized_sql_stmt;
}

static int
shard_stmt_change_shard_val_to_id (char **sql_stmt, const char **buf,
				   char appl_server)
{
  int error = NO_ERROR;
  SP_PARSER_HINT *hint_p = NULL;
  T_SHARD_KEY *key_p = NULL;
  T_SHARD_KEY_RANGE *range_p = NULL;
  const char *key_column;
  int shard_key_id = -1;
  char shard_key_id_string[14];

  hint_p = sp_create_parser_hint ();
  if (hint_p == NULL)
    {
      error = ER_SP_OUT_OF_MEMORY;
      goto FINALLY;
    }

  hint_p->hint_type = HT_VAL;

  *buf = sp_get_hint_arg (*buf, hint_p, &error);
  if (error != NO_ERROR)
    {
      goto FINALLY;
    }

  *buf = sp_check_end_of_hint (*buf, &error);
  if (error != NO_ERROR)
    {
      goto FINALLY;
    }

  key_p = (T_SHARD_KEY *) (&(shm_key_p->shard_key[0]));
  key_column = key_p->key_column;

  shard_key_id = proxy_find_shard_id_by_hint_value (&hint_p->arg, key_column);
  if (shard_key_id < 0)
    {
      error = ER_SP_INVALID_HINT;
      goto FINALLY;
    }

  range_p =
    shard_metadata_find_shard_range (shm_key_p, key_column, shard_key_id);
  if (range_p == NULL)
    {
      error = ER_SP_INVALID_HINT;

      PROXY_LOG (PROXY_LOG_MODE_ERROR, "Unable to find shard key range. "
		 "(key:[%s], key_id:%d).", key_column, shard_key_id);

      goto FINALLY;
    }

  sprintf (shard_key_id_string, "shard_id(%d)*/", range_p->shard_id);
  *sql_stmt = shard_stmt_write_buf_to_sql (*sql_stmt, shard_key_id_string,
					   strlen (shard_key_id_string),
					   true, appl_server);

FINALLY:
  if (hint_p != NULL)
    {
      sp_free_parser_hint (hint_p);
      free (hint_p);
    }
  return error;
}

char *
shard_stmt_write_buf_to_sql (char *sql_stmt, const char *buf, int length,
			     bool is_to_upper, char appl_server)
{
  int i = 0;

  for (; i < length; i++)
    {
#if 0
      if (is_to_upper && appl_server == APPL_SERVER_CAS)
	{
	  *(sql_stmt++) = toupper (*(buf++));
	}
      else
	{
	  *(sql_stmt++) = *(buf++);
	}
#else
      *(sql_stmt++) = *(buf++);
#endif
    }

  return sql_stmt;
}
