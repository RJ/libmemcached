/*
  This is a partial implementation for fetching/creating memcached_server_st objects.
*/
#include "common.h"

memcached_server_st *memcached_server_create(memcached_st *memc, memcached_server_st *ptr)
{
  if (ptr == NULL)
  {
    ptr= (memcached_server_st *)malloc(sizeof(memcached_server_st));

    if (!ptr)
      return NULL; /*  MEMCACHED_MEMORY_ALLOCATION_FAILURE */

    memset(ptr, 0, sizeof(memcached_server_st));
    ptr->is_allocated= true;
  }
  else
    memset(ptr, 0, sizeof(memcached_server_st));
  
  ptr->root= memc;

  return ptr;
}

memcached_server_st *memcached_server_create_with(memcached_st *memc, memcached_server_st *host, 
                                                  const char *hostname, unsigned int port, 
                                                  uint32_t weight, memcached_connection type)
{
  host= memcached_server_create(memc, host);

  if (host == NULL)
    return NULL;

  strncpy(host->hostname, hostname, MEMCACHED_MAX_HOST_LENGTH - 1);
  host->root= memc ? memc : NULL;
  host->port= port;
  host->weight= weight;
  host->fd= -1;
  host->type= type;
  host->read_ptr= host->read_buffer;
  if (memc)
    host->next_retry= memc->retry_timeout;

  return host;
}

void memcached_server_free(memcached_server_st *ptr)
{
  memcached_quit_server(ptr, 0);

  if (ptr->address_info)
  {
    freeaddrinfo(ptr->address_info);
    ptr->address_info= NULL;
  }

  if (ptr->is_allocated)
  {
    if (ptr->root && ptr->root->call_free)
      ptr->root->call_free(ptr->root, ptr);
    else
      free(ptr);
  }
  else
    memset(ptr, 0, sizeof(memcached_server_st));
}

/*
  If we do not have a valid object to clone from, we toss an error.
*/
memcached_server_st *memcached_server_clone(memcached_server_st *clone, memcached_server_st *ptr)
{
  /* We just do a normal create if ptr is missing */
  if (ptr == NULL)
    return NULL;

  /* TODO We should check return type */
  return memcached_server_create_with(ptr->root, clone, 
                                      ptr->hostname, ptr->port, ptr->weight,
                                      ptr->type);
}

memcached_return memcached_server_cursor(memcached_st *ptr, 
                                         memcached_server_function *callback,
                                         void *context,
                                         unsigned int number_of_callbacks)
{
  unsigned int y;

  for (y= 0; y < ptr->number_of_hosts; y++)
  {
    unsigned int x;

    for (x= 0; x < number_of_callbacks; x++)
    {
      unsigned int iferror;

      iferror= (*callback[x])(ptr, &ptr->hosts[y], context);

      if (iferror)
        continue;
    }
  }

  return MEMCACHED_SUCCESS;
}

memcached_server_st *memcached_server_by_key(memcached_st *ptr,  const char *key, size_t key_length, memcached_return *error)
{
  uint32_t server_key;

  *error= memcached_validate_key_length(key_length, 
                                        ptr->flags & MEM_BINARY_PROTOCOL);
  unlikely (*error != MEMCACHED_SUCCESS)
    return NULL;

  unlikely (ptr->number_of_hosts == 0)
  {
    *error= MEMCACHED_NO_SERVERS;
    return NULL;
  }

  if ((ptr->flags & MEM_VERIFY_KEY) && (memcachd_key_test((char **)&key, &key_length, 1) == MEMCACHED_BAD_KEY_PROVIDED))
  {
    *error= MEMCACHED_BAD_KEY_PROVIDED;
    return NULL;
  }

  server_key= memcached_generate_hash(ptr, key, key_length);

  return memcached_server_clone(NULL, &ptr->hosts[server_key]);

}
