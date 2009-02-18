#include "common.h"

static void calc_largest_consumption(memcached_analysis_st *result,
                                     const uint32_t server_num,
                                     const uint64_t nbytes)
{
  if (result->most_used_bytes < nbytes)
  {
    result->most_used_bytes= nbytes;
    result->most_consumed_server= server_num;
  }
}

static void calc_oldest_node(memcached_analysis_st *result,
                                     const uint32_t server_num,
                                     const uint32_t uptime)
{
  if (result->longest_uptime < uptime)
  {
    result->longest_uptime= uptime;
    result->oldest_server= server_num;
  }
}

static void calc_least_free_node(memcached_analysis_st *result,
                                 const uint32_t server_num,
                                 const long max_allowed_bytes,
                                 const long used_bytes)
{
  long remaining_bytes= max_allowed_bytes - used_bytes;

  if (result->least_remaining_bytes == 0 ||
      remaining_bytes < result->least_remaining_bytes)
  {
    result->least_remaining_bytes= remaining_bytes;
    result->least_free_server= server_num;
  }
}

static void calc_average_item_size(memcached_analysis_st *result,
                                   const uint64_t total_items,
                                   const uint64_t total_bytes)
{
  if (total_items > 0 && total_bytes > 0)
    result->average_item_size= total_bytes / total_items;
}

static void calc_hit_ratio(memcached_analysis_st *result,
                           const uint64_t total_get_hits,
                           const uint64_t total_get_cmds)
{
  if (total_get_hits == 0 || total_get_cmds == 0)
  {
    result->pool_hit_ratio= 0;
    return;
  }

  double temp= (double)total_get_hits/total_get_cmds;
  result->pool_hit_ratio= temp * 100;
}

memcached_analysis_st *memcached_analyze(memcached_st *memc,
                                         memcached_stat_st *stat,
                                         memcached_return *error)
{
  uint64_t total_items= 0, total_bytes= 0;
  uint64_t total_get_cmds= 0, total_get_hits= 0;
  uint32_t server_count, x;
  memcached_analysis_st *result;
  
  *error= MEMCACHED_SUCCESS;
  server_count= memcached_server_count(memc);
  result= (memcached_analysis_st*)malloc(sizeof(memcached_analysis_st)
                                         * (memc->number_of_hosts));
  if (!result)
  {
    *error= MEMCACHED_MEMORY_ALLOCATION_FAILURE;
    return NULL;
  }

  memset(result, 0, sizeof(*result));

  for (x= 0; x < server_count; x++)
  {
    calc_largest_consumption(result, x, stat[x].bytes);
    calc_oldest_node(result, x, stat[x].uptime);
    calc_least_free_node(result, x, stat[x].limit_maxbytes, stat[x].bytes);

    total_get_hits+= stat[x].get_hits;
    total_get_cmds+= stat[x].cmd_get;
    total_items+= stat[x].curr_items;
    total_bytes+= stat[x].bytes;
  }

  calc_average_item_size(result, total_items, total_bytes);
  calc_hit_ratio(result, total_get_hits, total_get_cmds);

  return result;
}
