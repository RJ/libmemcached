#include "common.h"

memcached_return memcachd_key_test(char **keys, size_t *key_length, 
                                   unsigned int number_of_keys)
{
  uint32_t x;
  memcached_return rc;

  for (x= 0; x < number_of_keys; x++)
  {
    size_t y;

    rc= memcached_validate_key_length(*(key_length + x), false);
    if (rc != MEMCACHED_SUCCESS)
      return rc;
    


    for (y= 0; y < *(key_length + x); y++)
    {
      if ((isgraph(keys[x][y])) == 0)
        return MEMCACHED_BAD_KEY_PROVIDED;
    }
  }

  return MEMCACHED_SUCCESS;
}

