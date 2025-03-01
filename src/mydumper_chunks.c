/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    Domas Mituzas, Facebook ( domas at fb dot com )
                    Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
                    Andrew Hutchings, MariaDB Foundation (andrew at mariadb dot org)
                    Max Bubenick, Percona RDBA (max dot bubenick at percona dot com)
                    David Ducos, Percona (david dot ducos at percona dot com)
*/
#include <mysql.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <math.h>
#include "common.h"
#include "mydumper_start_dump.h"
#include "server_detect.h"
#include "mydumper_chunks.h"
#include "mydumper_database.h"
#include "mydumper_jobs.h"
#include "mydumper_global.h"
#include "regex.h"
gboolean split_partitions = FALSE;
gchar *partition_regex = FALSE;
GAsyncQueue *give_me_another_innodb_chunk_step_queue;
GAsyncQueue *give_me_another_non_innodb_chunk_step_queue;
guint char_chunk=0;
guint char_deep=0;

void initialize_chunk(){
  give_me_another_innodb_chunk_step_queue=g_async_queue_new();
  give_me_another_non_innodb_chunk_step_queue=g_async_queue_new();

  if (rows_per_file>0){
    char_chunk=char_chunk==0?num_threads:char_chunk;
    char_deep=char_deep==0?num_threads:char_deep;
  }
}

void finalize_chunk(){
  g_async_queue_unref(give_me_another_innodb_chunk_step_queue); 
  g_async_queue_unref(give_me_another_non_innodb_chunk_step_queue);

}

guint64 gint64_abs(gint64 a){
  if (a >= 0)
    return a;
  return -a;
}

union chunk_step *new_char_step(MYSQL *conn, gchar *field, /*GList *list,*/ guint deep, guint number, MYSQL_ROW row, gulong *lengths){
  union chunk_step * cs = g_new0(union chunk_step, 1);

  cs->char_step.step=rows_per_file;

  cs->char_step.cmin_clen = lengths[2];
  cs->char_step.cmin_len = lengths[0]+1;
  cs->char_step.cmin = g_new(char, cs->char_step.cmin_len);
  g_strlcpy(cs->char_step.cmin, row[0], cs->char_step.cmin_len);
  cs->char_step.cmin_escaped = g_new(char, lengths[0] * 2 + 1);
  mysql_real_escape_string(conn, cs->char_step.cmin_escaped, row[0], lengths[0]);

  cs->char_step.cmax_clen = lengths[3];
  cs->char_step.cmax_len = lengths[1]+1;
  cs->char_step.cmax = g_new(char, cs->char_step.cmax_len);
  g_strlcpy(cs->char_step.cmax, row[1], cs->char_step.cmax_len);
  cs->char_step.cmax_escaped = g_new(char, lengths[1] * 2 + 1);
  mysql_real_escape_string(conn, cs->char_step.cmax_escaped, row[1], lengths[1]);

//  g_message("new_char_step: cmin: `%s` | cmax: `%s`", cs->char_step.cmin, cs->char_step.cmax);
  cs->char_step.assigned=FALSE;
  cs->char_step.deep = deep;
  cs->char_step.number = number;
  cs->char_step.mutex=g_mutex_new();
  cs->char_step.field = g_strdup(field);
  cs->char_step.previous=NULL;
//  cs->char_step.list = list; 

  cs->char_step.estimated_remaining_steps=1;
  cs->char_step.prefix=g_strdup_printf("`%s` IS NULL OR `%s` = '%s' OR", field, field, cs->char_step.cmin_escaped);

//  g_message("new_char_step: min: %s | max: %s ", cs->char_step.cmin_escaped, cs->char_step.cmax_escaped);

  cs->char_step.status = 0;
  return cs;
}


void next_chunk_in_char_step(union chunk_step * cs){
  cs->char_step.cmin_clen = cs->char_step.cursor_clen;
  cs->char_step.cmin_len = cs->char_step.cursor_len;
  cs->char_step.cmin = cs->char_step.cursor;
  cs->char_step.cmin_escaped = cs->char_step.cursor_escaped;
}

union chunk_step *split_char_step( guint deep, guint number, union chunk_step *previous_cs){
  union chunk_step * cs = g_new0(union chunk_step, 1);
  cs->char_step.prefix = NULL;
  cs->char_step.assigned=TRUE;
  cs->char_step.deep = deep;
  cs->char_step.number = number;
  cs->char_step.mutex=g_mutex_new();
  cs->char_step.step=rows_per_file;
  cs->char_step.field = g_strdup(previous_cs->char_step.field);
  cs->char_step.previous=previous_cs;
  cs->char_step.status = 0;
//  cs->char_step.list = list;
  return cs;
}

union chunk_step *new_integer_step(gchar *prefix, gchar *field, gboolean is_unsigned, union type type, guint deep, guint64 step, guint64 number, gboolean check_min, gboolean check_max){
//  g_message("New Integer Step with step size: %d", step);
  union chunk_step * cs = g_new0(union chunk_step, 1);
  cs->integer_step.is_unsigned = is_unsigned;
  cs->integer_step.prefix = prefix;
  cs->integer_step.step = step;
  if (cs->integer_step.is_unsigned){
//    g_message("Is unsigned... ");
    cs->integer_step.type.unsign.min = type.unsign.min;
    cs->integer_step.type.unsign.cursor = cs->integer_step.type.unsign.min;
    cs->integer_step.type.unsign.max = type.unsign.max;
    cs->integer_step.estimated_remaining_steps=(cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.min) / cs->integer_step.step;
  }else{
//    g_message("Is signed... ");
    cs->integer_step.type.sign.min = type.sign.min;
    cs->integer_step.type.sign.cursor = cs->integer_step.type.sign.min;
    cs->integer_step.type.sign.max = type.sign.max;
    cs->integer_step.estimated_remaining_steps=(cs->integer_step.type.sign.max - cs->integer_step.type.sign.min) / cs->integer_step.step;
  }
  cs->integer_step.deep = deep;
  cs->integer_step.number = number;
  cs->integer_step.field = g_strdup(field);
  cs->integer_step.mutex = g_mutex_new(); 
  cs->integer_step.status = UNASSIGNED;
  cs->integer_step.check_max=check_max;
  cs->integer_step.check_min=check_min;
  return cs;
}

union chunk_step *new_real_partition_step(GList *partition, guint deep, guint number){
  union chunk_step * cs = g_new0(union chunk_step, 1);
  cs->partition_step.list = partition;
  cs->partition_step.assigned= FALSE;
  cs->partition_step.mutex = g_mutex_new();
  cs->partition_step.deep = deep;
  cs->partition_step.number = number;
  return cs;
}

void free_char_step(union chunk_step * cs){
  g_mutex_lock(cs->char_step.mutex);
  g_free(cs->char_step.field);
  g_free(cs->char_step.prefix);
  g_mutex_unlock(cs->char_step.mutex);
  g_mutex_free(cs->char_step.mutex);
  g_free(cs);
}

void free_integer_step(union chunk_step * cs){
  if (cs->integer_step.field!=NULL){
    g_free(cs->integer_step.field);
    cs->integer_step.field=NULL;
  }
  if (cs->integer_step.prefix!=NULL)
    g_free(cs->integer_step.prefix);
  g_mutex_free(cs->integer_step.mutex); 
  g_free(cs);
}



void common_to_chunk_step(struct db_table *dbt, union chunk_step * cs, union chunk_step * new_cs){
  cs->integer_step.deep++;
  dbt->chunks=g_list_append(dbt->chunks,new_cs);

  new_cs->integer_step.status=ASSIGNED;

  g_async_queue_push(dbt->chunks_queue, cs);
  g_async_queue_push(dbt->chunks_queue, new_cs);

  g_mutex_unlock(cs->integer_step.mutex);
  g_mutex_unlock(dbt->chunks_mutex);
}


union chunk_step * split_unsigned_chunk_step(struct db_table *dbt, union chunk_step * cs){
  guint64 new_minmax = 0;
  union type type;
  type.unsign.max = cs->integer_step.type.unsign.max;
  union chunk_step * new_cs = NULL;
  if ( dbt->min_rows_per_file == dbt->start_rows_per_file && dbt->max_rows_per_file == dbt->start_rows_per_file){
    if ( cs->integer_step.status == DUMPING_CHUNK ){
      new_minmax = cs->integer_step.type.unsign.cursor + cs->integer_step.step *
                (( cs->integer_step.type.unsign.max    / cs->integer_step.step - 
                   cs->integer_step.type.unsign.cursor / cs->integer_step.step ) / 2 ) + 1;

      if (new_minmax == cs->integer_step.type.unsign.cursor)
      new_minmax = cs->integer_step.type.unsign.cursor + cs->integer_step.step * (
                 ( cs->integer_step.type.unsign.max    / cs->integer_step.step -
                   cs->integer_step.type.unsign.cursor / cs->integer_step.step ) / 2  + 1) + 1;

    }else{
      new_minmax = cs->integer_step.type.unsign.min    + cs->integer_step.step *
                (( cs->integer_step.type.unsign.max    / cs->integer_step.step - 
                   cs->integer_step.type.unsign.min    / cs->integer_step.step ) / 2 );

      if (new_minmax == cs->integer_step.type.unsign.min)
      new_minmax = cs->integer_step.type.unsign.min    + cs->integer_step.step * (
                 ( cs->integer_step.type.unsign.max    / cs->integer_step.step -
                   cs->integer_step.type.unsign.min    / cs->integer_step.step ) / 2 + 1);


    }
    type.unsign.min = new_minmax;
    new_cs = new_integer_step(NULL, dbt->field, cs->integer_step.is_unsigned, type, cs->integer_step.deep + 1, cs->integer_step.step, cs->integer_step.number, TRUE, cs->integer_step.check_max);
  }else{
    new_minmax = cs->integer_step.type.unsign.cursor + (cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.cursor)/2;
    if ( new_minmax == cs->integer_step.type.unsign.cursor )
      new_minmax++;
    type.unsign.min = new_minmax;
    new_cs = new_integer_step(NULL, dbt->field, cs->integer_step.is_unsigned, type, cs->integer_step.deep + 1, cs->integer_step.step, cs->integer_step.number+pow(2,cs->integer_step.deep), TRUE, cs->integer_step.check_max);
    cs->integer_step.check_max=TRUE;
    
  }

  cs->integer_step.type.unsign.max = new_minmax - 1;

  common_to_chunk_step(dbt, cs, new_cs);
  return new_cs;
}

union chunk_step * split_signed_chunk_step(struct db_table *dbt, union chunk_step * cs){
  gint64 new_minmax = 0;
  union type type;

  type.sign.max = cs->integer_step.type.sign.max;

  union chunk_step * new_cs = NULL;
  if ( dbt->min_rows_per_file == dbt->start_rows_per_file && dbt->max_rows_per_file == dbt->start_rows_per_file){
    if ( cs->integer_step.status == DUMPING_CHUNK ){
       new_minmax = cs->integer_step.type.sign.cursor + (signed) cs->integer_step.step *
                 (( cs->integer_step.type.sign.max    / (signed) cs->integer_step.step - 
                    cs->integer_step.type.sign.cursor / (signed) cs->integer_step.step ) / 2 ) + 1;
      if (new_minmax == cs->integer_step.type.sign.min)
       new_minmax = cs->integer_step.type.sign.min    + (signed) cs->integer_step.step *
                 (( cs->integer_step.type.sign.max    / (signed) cs->integer_step.step -
                    cs->integer_step.type.sign.min    / (signed) cs->integer_step.step ) / 2 + 1 ) + 1; 
   }else{
       new_minmax = cs->integer_step.type.sign.min    + (signed) cs->integer_step.step *
                 (( cs->integer_step.type.sign.max    / (signed) cs->integer_step.step - 
                    cs->integer_step.type.sign.min    / (signed) cs->integer_step.step ) / 2 );
      if (new_minmax == cs->integer_step.type.sign.min)
       new_minmax = cs->integer_step.type.sign.min    + (signed) cs->integer_step.step *
                 (( cs->integer_step.type.sign.max    / (signed) cs->integer_step.step -
                    cs->integer_step.type.sign.min    / (signed) cs->integer_step.step ) / 2 + 1);
    }
    type.sign.min = new_minmax;

    new_cs = new_integer_step(NULL, dbt->field, cs->integer_step.is_unsigned, type, cs->integer_step.deep + 1, cs->integer_step.step, cs->integer_step.number, TRUE, cs->integer_step.check_max);
  }else{
    new_minmax = gint64_abs(cs->integer_step.type.sign.max - cs->integer_step.type.sign.cursor) > cs->integer_step.step ?
                   cs->integer_step.type.sign.cursor + (cs->integer_step.type.sign.max - cs->integer_step.type.sign.cursor)/2 :
                   cs->integer_step.type.sign.cursor + 1;
    if ( new_minmax == cs->integer_step.type.sign.cursor )
      new_minmax++;
    type.sign.min = new_minmax;

    new_cs = new_integer_step(NULL, dbt->field, cs->integer_step.is_unsigned, type, cs->integer_step.deep + 1, cs->integer_step.step, cs->integer_step.number+pow(2,cs->integer_step.deep), TRUE, cs->integer_step.check_max);

    cs->integer_step.check_max=TRUE;
  }

  cs->integer_step.type.sign.max = new_minmax - 1;

  common_to_chunk_step(dbt, cs, new_cs);

  return new_cs;
}



union chunk_step *get_next_integer_chunk(struct db_table *dbt){
  g_mutex_lock(dbt->chunks_mutex);
//  GList *l=dbt->chunks;
  union chunk_step *cs=NULL;
  if (dbt->chunks!=NULL){
//    g_message("IN WHILE");
//    cs=l->data;
    cs = (union chunk_step *)g_async_queue_try_pop(dbt->chunks_queue);      
    while (cs!=NULL){
      g_mutex_lock(cs->integer_step.mutex);
      if (cs->integer_step.status==UNASSIGNED){
//      g_message("Not assigned");
        cs->integer_step.status=ASSIGNED;
        g_async_queue_push(dbt->chunks_queue, cs);
        g_mutex_unlock(cs->integer_step.mutex);
        g_mutex_unlock(dbt->chunks_mutex);
        return cs;
      }
      if (cs->integer_step.is_unsigned) {

        if (cs->integer_step.type.unsign.cursor < cs->integer_step.type.unsign.max // it is not the last chunk
        && (  
             ( cs->integer_step.status == DUMPING_CHUNK && cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.cursor > cs->integer_step.step // As this chunk is dumping data, another thread can continue with the remaining rows
             ) || 
             ( cs->integer_step.status == ASSIGNED      && cs->integer_step.type.unsign.max - cs->integer_step.type.unsign.min    > cs->integer_step.step // As this chunk is going to process another step, another thread can continue with the remaining rows
             )
           )
         ){
          return split_unsigned_chunk_step(dbt,cs);
        }else{
//        g_message("Not able to split min %"G_GUINT64_FORMAT" step: %"G_GUINT64_FORMAT" max: %"G_GUINT64_FORMAT, cs->integer_step.nmin, cs->integer_step.step, cs->integer_step.nmax);
          g_mutex_unlock(cs->integer_step.mutex);
          if (cs->integer_step.status==COMPLETED){
            free_integer_step(cs);
          }
        }
      }else{
        if (cs->integer_step.type.sign.cursor < cs->integer_step.type.sign.max // it is not the last chunk
        && (  
             ( cs->integer_step.status == DUMPING_CHUNK && gint64_abs(cs->integer_step.type.sign.max - cs->integer_step.type.sign.cursor) > cs->integer_step.step // As this chunk is dumping data, another thread can continue with the remaining rows
             ) || 
             ( cs->integer_step.status == ASSIGNED      && gint64_abs(cs->integer_step.type.sign.max - cs->integer_step.type.sign.min)    > cs->integer_step.step // As this chunk is going to process another step, another thread can continue with the remaining rows
             )
           )
         ){
          return split_signed_chunk_step(dbt,cs);
        }else{
//        g_message("Not able to split min %"G_GUINT64_FORMAT" step: %"G_GUINT64_FORMAT" max: %"G_GUINT64_FORMAT, cs->integer_step.nmin, cs->integer_step.step, cs->integer_step.nmax);
          g_mutex_unlock(cs->integer_step.mutex);
          if (cs->integer_step.status==COMPLETED){
            free_integer_step(cs);
          }
        }
      }
      cs = (union chunk_step *)g_async_queue_try_pop(dbt->chunks_queue);
    }

//    g_mutex_unlock(cs->integer_step.mutex);
//    l=l->next;
  }
  g_mutex_unlock(dbt->chunks_mutex);
  return NULL;
}

union chunk_step *get_next_char_chunk(struct db_table *dbt){
  g_mutex_lock(dbt->chunks_mutex);
  GList *l=dbt->chunks;
  union chunk_step *cs=NULL;
  while (l!=NULL){
    cs=l->data;
    if (cs->char_step.mutex == NULL){
      g_message("This should not happen");
      l=l->next;
      continue;
    }
    
    g_mutex_lock(cs->char_step.mutex);
    if (!cs->char_step.assigned){
      cs->char_step.assigned=TRUE;
      g_mutex_unlock(cs->char_step.mutex);
      g_mutex_unlock(dbt->chunks_mutex);
      return cs;
    }
    if (cs->char_step.deep <= char_deep && g_strcmp0(cs->char_step.cmax, cs->char_step.cursor)!=0 && cs->char_step.status == 0){
      union chunk_step * new_cs = split_char_step(
          cs->char_step.deep + 1, cs->char_step.number+pow(2,cs->char_step.deep), cs);
      cs->char_step.deep++;
      cs->char_step.status = 1;
      new_cs->char_step.assigned=TRUE;
      return new_cs;
    }else{
//      g_message("Not able to split because %d > %d | %s == %s | %d != 0", cs->char_step.deep,num_threads, cs->char_step.cmax, cs->char_step.cursor, cs->char_step.status);
    }
    g_mutex_unlock(cs->char_step.mutex);
    l=l->next;
  }
  g_mutex_unlock(dbt->chunks_mutex);
  return NULL;
}

union chunk_step *get_next_partition_chunk(struct db_table *dbt){
  g_mutex_lock(dbt->chunks_mutex);
  GList *l=dbt->chunks;
  union chunk_step *cs=NULL;
  while (l!=NULL){
    cs=l->data;
    g_mutex_lock(cs->partition_step.mutex);
    if (!cs->partition_step.assigned){
      cs->partition_step.assigned=TRUE;
      g_mutex_unlock(cs->partition_step.mutex);
      g_mutex_unlock(dbt->chunks_mutex);
      return cs;
    }

    if (g_list_length (cs->partition_step.list) > 3 ){
      guint pos=g_list_length (cs->partition_step.list) / 2;
      GList *new_list=g_list_nth(cs->partition_step.list,pos);
      new_list->prev->next=NULL;
      new_list->prev=NULL;
      union chunk_step * new_cs = new_real_partition_step(new_list, cs->partition_step.deep+1, cs->partition_step.number+pow(2,cs->partition_step.deep));
      cs->partition_step.deep++;
      new_cs->partition_step.assigned=TRUE;
      dbt->chunks=g_list_append(dbt->chunks,new_cs);

      g_mutex_unlock(cs->partition_step.mutex);
      g_mutex_unlock(dbt->chunks_mutex);
      return new_cs;
    }
    g_mutex_unlock(cs->partition_step.mutex);
    l=l->next;
  }
  g_mutex_unlock(dbt->chunks_mutex);
  return NULL;
}

union chunk_step *get_next_chunk(struct db_table *dbt){
  switch (dbt->chunk_type){
    case CHAR: 
      return get_next_char_chunk(dbt);
      break;
    case INTEGER:
      return get_next_integer_chunk(dbt);
      break;
    case PARTITION:
      return get_next_partition_chunk(dbt);
      break;
    default:
      break;
  }
  return NULL;
}

GList * get_partitions_for_table(MYSQL *conn, struct db_table *dbt){
  MYSQL_RES *res=NULL;
  MYSQL_ROW row;

  GList *partition_list = NULL;

  gchar *query = g_strdup_printf("select PARTITION_NAME from information_schema.PARTITIONS where PARTITION_NAME is not null and TABLE_SCHEMA='%s' and TABLE_NAME='%s'", dbt->database->name, dbt->table);
  mysql_query(conn,query);
  g_free(query);

  res = mysql_store_result(conn);
  if (res == NULL)
    //partitioning is not supported
    return partition_list;
  while ((row = mysql_fetch_row(res))) {
    if ( (!dbt->partition_regex && eval_partition_regex(row[0])) || (dbt->partition_regex && eval_pcre_regex(dbt->partition_regex, row[0]) ) )
      partition_list = g_list_append(partition_list, strdup(row[0]));
  }
  mysql_free_result(res);

  return partition_list;
}

gchar * get_escaped_middle_char(MYSQL *conn, gchar *c1, guint c1len, gchar *c2, guint c2len, guint part){
  guint cresultlen = c1len < c2len ? c1len: c2len;
  gchar *cresult = g_new(gchar, cresultlen + 1);
  guint i =0;
  guchar cu1=c1[0],cu2=c2[0];
//  g_message("get_escaped_middle_char: %u %u %u %d", cu1, abs(cu2-cu1) , cu2, part);
  for(i=0; i < cresultlen; i++){
    cu1=c1[i];
    cu2=c2[i];
    if (cu2!=cu1)
      cresult[i]=(cu2>cu1?cu1:cu2)+abs(cu2-cu1)/part;
    else
      cresultlen=i;
  }
  cu1=c1[0];cu2=c2[0];
//  guchar cur=cresult[0];
//  g_message("get_escaped_middle_char: %u %u %u %d", cu1, cur , cu2, part);
  cresult[cresultlen]='\0';

  gchar *escapedresult=g_new(char, cresultlen * 2 + 1);
  mysql_real_escape_string(conn, escapedresult, cresult, cresultlen);
  g_free(cresult);
  return escapedresult;
}

void update_integer_min(MYSQL *conn, struct table_job *tj){
  union chunk_step *cs= tj->chunk_step;
  gchar *query = NULL;
  MYSQL_ROW row = NULL;
  MYSQL_RES *minmax = NULL;
  /* Get minimum/maximum */


if (cs->integer_step.is_unsigned) {

  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE %s %"G_GUINT64_FORMAT" <= `%s` AND `%s` <= %"G_GUINT64_FORMAT" ORDER BY `%s` ASC LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        tj->dbt->field, tj->dbt->database->name, tj->dbt->table, cs->integer_step.prefix,  cs->integer_step.type.unsign.min, tj->dbt->field, tj->dbt->field, cs->integer_step.type.unsign.max, tj->dbt->field));

}else{

  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE %s %"G_GINT64_FORMAT" <= `%s` AND `%s` <= %"G_GINT64_FORMAT" ORDER BY `%s` ASC LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        tj->dbt->field, tj->dbt->database->name, tj->dbt->table, cs->integer_step.prefix,  cs->integer_step.type.sign.min, tj->dbt->field, tj->dbt->field, cs->integer_step.type.sign.max, tj->dbt->field));

}

  g_free(query);
  minmax = mysql_store_result(conn);

  if (!minmax){
    return;
  }
  row = mysql_fetch_row(minmax);

  if (row==NULL || row[0]==NULL){
    return;
  }
if (cs->integer_step.is_unsigned) {
  guint64 nmin = strtoull(row[0], NULL, 10);
  cs->integer_step.type.unsign.min = nmin;
}else{
  gint64 nmin = strtoll(row[0], NULL, 10);
  cs->integer_step.type.sign.min = nmin;

}
}

void update_integer_max(MYSQL *conn, struct table_job *tj){
  union chunk_step *cs= tj->chunk_step;
  gchar *query = NULL;
  MYSQL_ROW row = NULL;
  MYSQL_RES *minmax = NULL;
  /* Get minimum/maximum */


if (cs->integer_step.is_unsigned) {
  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE %"G_GUINT64_FORMAT" <= `%s` AND `%s` <= %"G_GUINT64_FORMAT" ORDER BY `%s` DESC LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        tj->dbt->field, tj->dbt->database->name, tj->dbt->table, cs->integer_step.type.unsign.min, tj->dbt->field, tj->dbt->field, cs->integer_step.type.unsign.max, tj->dbt->field));

}else{
  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE %"G_GINT64_FORMAT" <= `%s` AND `%s` <= %"G_GINT64_FORMAT" ORDER BY `%s` DESC LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        tj->dbt->field, tj->dbt->database->name, tj->dbt->table, cs->integer_step.type.sign.min, tj->dbt->field, tj->dbt->field, cs->integer_step.type.sign.max, tj->dbt->field));


}

//  g_free(query);
  minmax = mysql_store_result(conn);
  g_free(query);

  if (!minmax){
//    g_message("No middle point");
    goto cleanup;
  }
  row = mysql_fetch_row(minmax);

  if (row==NULL || row[0]==NULL){
//    g_message("No middle point");
cleanup:

    if (cs->integer_step.is_unsigned) {
      cs->integer_step.type.unsign.max = cs->integer_step.type.unsign.min;
    }else{
      cs->integer_step.type.sign.max = cs->integer_step.type.sign.min;
    }

    mysql_free_result(minmax);
    return;
  }

if (cs->integer_step.is_unsigned) {
  guint64 nmax = strtoull(row[0], NULL, 10);
  cs->integer_step.type.unsign.max = nmax;
}else{
  gint64 nmax = strtoll(row[0], NULL, 10);
  cs->integer_step.type.sign.max = nmax;

}


  mysql_free_result(minmax);
}

gchar* update_cursor (MYSQL *conn, struct table_job *tj){
  union chunk_step *cs= tj->chunk_step;
  gchar *query = NULL;
  MYSQL_ROW row;
  MYSQL_RES *minmax = NULL;
  /* Get minimum/maximum */
  gchar * middle = get_escaped_middle_char(conn, cs->char_step.cmax, cs->char_step.cmax_clen, cs->char_step.cmin, cs->char_step.cmin_clen, tj->char_chunk_part>0?tj->char_chunk_part:1);//num_threads*(num_threads - cs->char_step.deep>0?num_threads-cs->char_step.deep:1));
  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE '%s' <= `%s` AND '%s' <= `%s` AND `%s` <= '%s' ORDER BY `%s` LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        tj->dbt->field, tj->dbt->database->name, tj->dbt->table, cs->char_step.cmin_escaped, tj->dbt->field, middle, tj->dbt->field, tj->dbt->field, cs->char_step.cmax_escaped, tj->dbt->field));
  g_free(query);
  minmax = mysql_store_result(conn);

  if (!minmax){
//    g_message("No middle point");
    goto cleanup;
  }
  row = mysql_fetch_row(minmax);

  if (row==NULL){
//    g_message("No middle point");
cleanup:
    cs->char_step.cursor_clen = cs->char_step.cmax_clen;
    cs->char_step.cursor_len = cs->char_step.cmax_len;
    cs->char_step.cursor = cs->char_step.cmax;
    cs->char_step.cursor_escaped = cs->char_step.cmax_escaped;
    return NULL;
  }
//  guchar d=middle[0];
//  g_message("updated point: `%s` | `%c` %u", middle, middle[0], d);
  gulong *lengths = mysql_fetch_lengths(minmax);

  tj->char_chunk_part--;

  if (g_strcmp0(row[0], cs->char_step.cmax)!=0 && g_strcmp0(row[0], cs->char_step.cmin)!=0){
    cs->char_step.cursor_clen = lengths[0];
    cs->char_step.cursor_len = lengths[0]+1;
    cs->char_step.cursor = g_new(char, cs->char_step.cursor_len);
    g_strlcpy(cs->char_step.cursor, row[0], cs->char_step.cursor_len);
    cs->char_step.cursor_escaped = g_new(char, lengths[0] * 2 + 1);
    mysql_real_escape_string(conn, cs->char_step.cursor_escaped, row[0], lengths[0]);
  }else{
    cs->char_step.cursor_clen = cs->char_step.cmax_clen;
    cs->char_step.cursor_len = cs->char_step.cmax_len;
    cs->char_step.cursor = cs->char_step.cmax;
    cs->char_step.cursor_escaped = cs->char_step.cmax_escaped;
  }

  return NULL;
}

gboolean get_new_minmax (struct thread_data *td, struct db_table *dbt, union chunk_step *cs){
//  g_message("Thread %d: get_new_minmax", td->thread_id);
  gchar *query = NULL;
  MYSQL_ROW row;
  MYSQL_RES *minmax = NULL;
  union chunk_step * previous=cs->char_step.previous;
  /* Get minimum/maximum */

  gchar *middle=get_escaped_middle_char(td->thrconn, previous->char_step.cmax, previous->char_step.cmax_clen, previous->char_step.cursor != NULL ? previous->char_step.cursor: previous->char_step.cmin, previous->char_step.cursor != NULL ?previous->char_step.cursor_len:previous->char_step.cmin_clen, char_chunk);
//  guchar d=middle[0];
//  g_message("Middle point: `%s` | `%c` %u", middle, middle[0], d);
  mysql_query(td->thrconn, query = g_strdup_printf(
                        "SELECT %s `%s` FROM `%s`.`%s` WHERE `%s` > (SELECT `%s` FROM `%s`.`%s` WHERE `%s` > '%s' ORDER BY `%s` LIMIT 1) AND '%s' < `%s` AND `%s` < '%s' ORDER BY `%s` LIMIT 1",
                        is_mysql_like() ? "/*!40001 SQL_NO_CACHE */": "",
                        dbt->field, dbt->database->name, dbt->table, dbt->field, dbt->field, dbt->database->name, dbt->table, dbt->field, middle, dbt->field, previous->char_step.cursor_escaped!=NULL?previous->char_step.cursor_escaped:previous->char_step.cmin_escaped, dbt->field, dbt->field, previous->char_step.cmax_escaped, dbt->field));
  g_free(query);
  minmax = mysql_store_result(td->thrconn);

  if (!minmax){
    mysql_free_result(minmax);
//    g_message("No middle point");
    return FALSE;
  }

  row = mysql_fetch_row(minmax);
  if (row == NULL){
    mysql_free_result(minmax);
//    g_message("No middle point");
    return FALSE;
  }
//  guchar c=row[0][0];
//  g_message("First char %u ", c);
  gulong *lengths = mysql_fetch_lengths(minmax);

  cs->char_step.cmax_clen = previous->char_step.cmax_clen;
  cs->char_step.cmax_len = previous->char_step.cmax_len;
  cs->char_step.cmax = previous->char_step.cmax;
  cs->char_step.cmax_escaped = previous->char_step.cmax_escaped;
  
  previous->char_step.cmax_clen = lengths[0];
  previous->char_step.cmax_len = lengths[0]+1;
  previous->char_step.cmax = g_new(char, previous->char_step.cmax_len);
  g_strlcpy(previous->char_step.cmax, row[0], previous->char_step.cmax_len);
  previous->char_step.cmax_escaped = g_new(char, lengths[0] * 2 + 1);
  mysql_real_escape_string(td->thrconn, previous->char_step.cmax_escaped, row[0], lengths[0]);

  previous->char_step.status=0;

  cs->char_step.cmin_clen = lengths[0];
  cs->char_step.cmin_len = lengths[0]+1;
  cs->char_step.cmin = g_new(char, cs->char_step.cmin_len);
  g_strlcpy(cs->char_step.cmin, row[0], cs->char_step.cmin_len);
  cs->char_step.cmin_escaped = g_new(char, lengths[0] * 2 + 1);
  mysql_real_escape_string(td->thrconn, cs->char_step.cmin_escaped, row[0], lengths[0]);

  mysql_free_result(minmax);
  return TRUE;
}

void set_chunk_strategy_for_dbt(MYSQL *conn, struct db_table *dbt){
  GList *partitions=NULL;
  if (split_partitions || dbt->partition_regex){
    partitions = get_partitions_for_table(conn, dbt);
  }

  if (partitions){
    dbt->chunks=g_list_prepend(dbt->chunks,new_real_partition_step(partitions,0,0));
    dbt->chunk_type=PARTITION;
    return;
  }

  if (dbt->start_rows_per_file>0 && dbt->rows_in_sts > dbt->min_rows_per_file ){
    gchar *query = NULL;
    MYSQL_ROW row;
    MYSQL_RES *minmax = NULL;
    /* Get minimum/maximum */
    mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s MIN(`%s`),MAX(`%s`),LEFT(MIN(`%s`),1),LEFT(MAX(`%s`),1) FROM `%s`.`%s` %s %s",
                        is_mysql_like()
                            ? "/*!40001 SQL_NO_CACHE */"
                            : "",
                        dbt->field, dbt->field, dbt->field, dbt->field, dbt->database->name, dbt->table, where_option ? "WHERE" : "", where_option ? where_option : ""));
//  g_message("Query: %s", query);
    g_free(query);
    minmax = mysql_store_result(conn);

    if (!minmax){
      dbt->chunk_type=NONE;
      goto cleanup;
    }

    row = mysql_fetch_row(minmax);

    MYSQL_FIELD *fields = mysql_fetch_fields(minmax);
    gulong *lengths = mysql_fetch_lengths(minmax);
    /* Check if all values are NULL */
    if (row[0] == NULL){
      dbt->chunk_type=NONE;
      goto cleanup;
    }
  /* Support just bigger INTs for now, very dumb, no verify approach */
    guint64 abs;
    guint64 unmin, unmax;
    gint64 nmin, nmax;
    gchar *prefix=NULL;
    union chunk_step *cs = NULL;
    switch (fields[0].type) {
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_SHORT:
        if (dbt->min_rows_per_file==dbt->start_rows_per_file && dbt->max_rows_per_file==dbt->start_rows_per_file)
          dbt->chunk_filesize=0;

        unmin = strtoull(row[0], NULL, 10);
        unmax = strtoull(row[1], NULL, 10) + 1;
        nmin  = strtoll (row[0], NULL, 10);
        nmax  = strtoll (row[1], NULL, 10) + 1;

        if (fields[0].flags & UNSIGNED_FLAG){
          prefix= g_strdup_printf("`%s` IS NULL OR", dbt->field) ;
          abs=gint64_abs(unmax-unmin);
        }else{
          prefix= g_strdup_printf("`%s` IS NULL OR ", dbt->field) ;
          abs=gint64_abs(nmax-nmin);
        }
        if ( abs > dbt->min_rows_per_file){
          union type type;
          if ((fields[0].flags & UNSIGNED_FLAG)){
            type.unsign.min=unmin;
            type.unsign.max=unmax;
            cs=new_integer_step(prefix, dbt->field, fields[0].flags & UNSIGNED_FLAG, type, 0, dbt->start_rows_per_file, 0, FALSE, FALSE);
          }else{
            type.sign.min=nmin;
            type.sign.max=nmax;
            cs=new_integer_step(prefix, dbt->field, fields[0].flags & UNSIGNED_FLAG, type, 0, dbt->start_rows_per_file, 0, FALSE, FALSE);
          }

          dbt->chunks=g_list_prepend(dbt->chunks,cs);
          g_async_queue_push(dbt->chunks_queue, cs);
          dbt->chunk_type=INTEGER;
          dbt->estimated_remaining_steps=cs->integer_step.estimated_remaining_steps;
        }else{
          dbt->chunk_type=NONE;
        }
        if (minmax) mysql_free_result(minmax);
        return;
        break;
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VAR_STRING:
        cs=new_char_step(conn, dbt->field, 0, 0, row, lengths);
        dbt->chunks=g_list_prepend(dbt->chunks,cs);
        g_async_queue_push(dbt->chunks_queue, cs);
        dbt->chunk_type=CHAR;
        if (minmax) mysql_free_result(minmax);
        return;
        break;
      default:
        dbt->chunk_type=NONE;
        break;
      }
    cleanup:
      if (minmax)
        mysql_free_result(minmax);
  }else
    dbt->chunk_type=NONE;
}

char *get_field_for_dbt(MYSQL *conn, struct db_table * dbt, struct configuration *conf){
  MYSQL_RES *indexes = NULL;
  MYSQL_ROW row;
  char *field = NULL;

  /* first have to pick index, in future should be able to preset in
 *    * configuration too */
  gchar *query = g_strdup_printf("SHOW INDEX FROM `%s`.`%s`", dbt->database->name, dbt->table);
  mysql_query(conn, query);
  g_free(query);
  indexes = mysql_store_result(conn);

  if (indexes){
    while ((row = mysql_fetch_row(indexes))) {
      if (!strcmp(row[2], "PRIMARY") && (!strcmp(row[3], "1"))) {
        /* Pick first column in PK, cardinality doesn't matter */
        field = g_strdup(row[4]);
        break;
      }
    }

    /* If no PK found, try using first UNIQUE index */
    if (!field) {
      mysql_data_seek(indexes, 0);
      while ((row = mysql_fetch_row(indexes))) {
        if (!strcmp(row[1], "0") && (!strcmp(row[3], "1"))) {
          /* Again, first column of any unique index */
          field = g_strdup(row[4]);
          break;
        }
      }
    }
    /* Still unlucky? Pick any high-cardinality index */
    if (!field && conf->use_any_index) {
      guint64 max_cardinality = 0;
      guint64 cardinality = 0;

      mysql_data_seek(indexes, 0);
      while ((row = mysql_fetch_row(indexes))) {
        if (!strcmp(row[3], "1")) {
          if (row[6])
            cardinality = strtoul(row[6], NULL, 10);
          if (cardinality > max_cardinality) {
            field = g_strdup(row[4]);
            max_cardinality = cardinality;
          }
        }
      }
    }
  }
  if (indexes)
    mysql_free_result(indexes);
  return field;
}

gboolean get_next_dbt_and_chunk(struct db_table **dbt,union chunk_step **cs, GList **dbt_list){
  GList *iter=*dbt_list;
  union chunk_step *lcs;
  struct db_table *d;
  gboolean are_there_jobs_defining=FALSE;
  while (iter){
    d=iter->data;
    if (d->chunk_type != DEFINING){
      if (d->chunk_type == NONE){
        *dbt=iter->data;
        *dbt_list=g_list_remove(*dbt_list,d);
        break;
      }
      if (d->chunk_type == UNDEFINED){
        *dbt=iter->data;
        d->chunk_type = DEFINING;
        are_there_jobs_defining=TRUE;
        break;
      }
      lcs=get_next_chunk(d);
      if (lcs!=NULL){
        *cs=lcs;
        *dbt=iter->data;
        break;
      }else{
        iter=iter->next;
        *dbt_list=g_list_remove(*dbt_list,d);
        continue;
      }
    }else{
      are_there_jobs_defining=TRUE;
    }
    iter=iter->next;
  }
  return are_there_jobs_defining;
}

void give_me_another_non_innodb_chunk_step(){
  g_async_queue_push(give_me_another_non_innodb_chunk_step_queue, GINT_TO_POINTER(1));
}

void give_me_another_innodb_chunk_step(){
  g_async_queue_push(give_me_another_innodb_chunk_step_queue, GINT_TO_POINTER(1));
}

void enqueue_shutdown_jobs(GAsyncQueue * queue){
  struct job *j=NULL;
  guint n;
  for (n = 0; n < num_threads; n++) {
    j = g_new0(struct job, 1);
    j->type = JOB_SHUTDOWN;
    g_async_queue_push(queue, j);
  }
}

void table_job_enqueue(GAsyncQueue * pop_queue, GAsyncQueue * push_queue, GList **table_list){
  struct db_table *dbt;
  union chunk_step *cs;
  gboolean are_there_jobs_defining=FALSE;
  for (;;) {
    g_async_queue_pop(pop_queue);
    if (shutdown_triggered) {
      return; 
    }
    dbt=NULL;
    cs=NULL;
    are_there_jobs_defining=FALSE;
    are_there_jobs_defining=get_next_dbt_and_chunk(&dbt,&cs,table_list);

    if ((cs==NULL) && (dbt==NULL)){
      if (are_there_jobs_defining){
        g_debug("chunk_builder_thread: Are jobs defining... should we wait and try again later?");
        g_async_queue_push(pop_queue, GINT_TO_POINTER(1));
        usleep(1);
        continue;
      }
      g_debug("chunk_builder_thread: There were not job defined");
      break;
    }
    g_debug("chunk_builder_thread: Job will be enqueued");
    switch (dbt->chunk_type) {
    case INTEGER:
      create_job_to_dump_chunk(dbt, NULL, cs->integer_step.number, dbt->primary_key, cs, g_async_queue_push, push_queue, TRUE);
      break;
    case CHAR:
      create_job_to_dump_chunk(dbt, NULL, cs->char_step.number, dbt->primary_key, cs, g_async_queue_push, push_queue, FALSE);
      break;
    case PARTITION:
      create_job_to_dump_chunk(dbt, NULL, cs->partition_step.number, dbt->primary_key, cs, g_async_queue_push, push_queue, TRUE);
      break;
    case NONE:
      create_job_to_dump_chunk(dbt, NULL, 0, dbt->primary_key, cs, g_async_queue_push, push_queue, TRUE);
      break;
    case DEFINING:
      create_job_to_determine_chunk_type(dbt, g_async_queue_push, push_queue);
      break;
    default:
      m_error("This should not happen");
      break;
    } 
  }
}

void *chunk_builder_thread(struct configuration *conf){

  g_message("Starting Non-InnoDB tables");
  table_job_enqueue(give_me_another_non_innodb_chunk_step_queue, conf->non_innodb_queue, &non_innodb_table);
  g_message("Non-InnoDB tables completed");
  enqueue_shutdown_jobs(conf->non_innodb_queue);

  g_message("Starting InnoDB tables");
  table_job_enqueue(give_me_another_innodb_chunk_step_queue, conf->innodb_queue, &innodb_table);
  g_message("InnoDB tables completed");
  enqueue_shutdown_jobs(conf->innodb_queue);

  return NULL;
}

