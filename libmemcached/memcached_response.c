/*
  Memcached library

  memcached_response() is used to determine the return result
  from an issued command.
*/

#include "common.h"
#include "memcached_io.h"

static memcached_return textual_read_one_response(memcached_server_st *ptr,
                                                  char *buffer, size_t buffer_length,
                                                  memcached_result_st *result);
static memcached_return binary_read_one_response(memcached_server_st *ptr,
                                                 char *buffer, size_t buffer_length,
                                                 memcached_result_st *result);

memcached_return memcached_read_one_response(memcached_server_st *ptr,
                                             char *buffer, size_t buffer_length,
                                             memcached_result_st *result)
{
  memcached_server_response_decrement(ptr);

  memcached_return rc;
  if (ptr->root->flags & MEM_BINARY_PROTOCOL)
    rc= binary_read_one_response(ptr, buffer, buffer_length, result);
  else
    rc= textual_read_one_response(ptr, buffer, buffer_length, result);

  unlikely(rc == MEMCACHED_UNKNOWN_READ_FAILURE ||
           rc == MEMCACHED_PROTOCOL_ERROR ||
           rc == MEMCACHED_CLIENT_ERROR ||
           rc == MEMCACHED_MEMORY_ALLOCATION_FAILURE)
     memcached_io_reset(ptr);

  return rc;
}

memcached_return memcached_response(memcached_server_st *ptr, 
                                    char *buffer, size_t buffer_length,
                                    memcached_result_st *result)
{
  /* We may have old commands in the buffer not set, first purge */
  if (ptr->root->flags & MEM_NO_BLOCK)
    (void)memcached_io_write(ptr, NULL, 0, 1);

  /*
   * The previous implementation purged all pending requests and just
   * returned the last one. Purge all pending messages to ensure backwards
   * compatibility. 
   */
  if ((ptr->root->flags & MEM_BINARY_PROTOCOL) == 0)
    while (memcached_server_response_count(ptr) > 1) {
      memcached_return rc= memcached_read_one_response(ptr, buffer, buffer_length, result);
      
      unlikely (rc != MEMCACHED_END &&
                rc != MEMCACHED_STORED &&
                rc != MEMCACHED_SUCCESS &&
                rc != MEMCACHED_STAT && 
                rc != MEMCACHED_DELETED &&
                rc != MEMCACHED_NOTFOUND &&
                rc != MEMCACHED_NOTSTORED && 
                rc != MEMCACHED_DATA_EXISTS)
	return rc;
    }

  return memcached_read_one_response(ptr, buffer, buffer_length, result);
}

static memcached_return textual_read_one_response(memcached_server_st *ptr,
                                                  char *buffer, size_t buffer_length,
                                                  memcached_result_st *result)
{
  memcached_return rc= memcached_io_readline(ptr, buffer, buffer_length);
  if (rc != MEMCACHED_SUCCESS)
    return rc;

  switch(buffer[0])
  {
  case 'V': /* VALUE || VERSION */
    if (buffer[1] == 'A') /* VALUE */
    {
      memcached_return rc;

      /* We add back in one because we will need to search for END */
      memcached_server_response_increment(ptr);
      if (result)
        rc= value_fetch(ptr, buffer, result);
      else
        rc= value_fetch(ptr, buffer, &ptr->root->result);

      return rc;
    }
    else if (buffer[1] == 'E') /* VERSION */
    {
      return MEMCACHED_SUCCESS;
    }
    else
    {
      WATCHPOINT_STRING(buffer);
      WATCHPOINT_ASSERT(0);
      return MEMCACHED_UNKNOWN_READ_FAILURE;
    }
  case 'O': /* OK */
    return MEMCACHED_SUCCESS;
  case 'S': /* STORED STATS SERVER_ERROR */
    {
      if (buffer[2] == 'A') /* STORED STATS */
      {
        memcached_server_response_increment(ptr);
        return MEMCACHED_STAT;
      }
      else if (buffer[1] == 'E')
        return MEMCACHED_SERVER_ERROR;
      else if (buffer[1] == 'T')
        return MEMCACHED_STORED;
      else
      {
        WATCHPOINT_STRING(buffer);
        WATCHPOINT_ASSERT(0);
        return MEMCACHED_UNKNOWN_READ_FAILURE;
      }
    }
  case 'D': /* DELETED */
    return MEMCACHED_DELETED;
  case 'N': /* NOT_FOUND */
    {
      if (buffer[4] == 'F')
        return MEMCACHED_NOTFOUND;
      else if (buffer[4] == 'S')
        return MEMCACHED_NOTSTORED;
      else
        return MEMCACHED_UNKNOWN_READ_FAILURE;
    }
  case 'E': /* PROTOCOL ERROR or END */
    {
      if (buffer[1] == 'N')
        return MEMCACHED_END;
      else if (buffer[1] == 'R')
        return MEMCACHED_PROTOCOL_ERROR;
      else if (buffer[1] == 'X')
        return MEMCACHED_DATA_EXISTS;
      else
        return MEMCACHED_UNKNOWN_READ_FAILURE;
    }
  case 'C': /* CLIENT ERROR */
    return MEMCACHED_CLIENT_ERROR;
  default:
    {
      unsigned long long auto_return_value;

      if (sscanf(buffer, "%llu", &auto_return_value) == 1)
        return MEMCACHED_SUCCESS;

      return MEMCACHED_UNKNOWN_READ_FAILURE;
    }
  }

  /* NOTREACHED */
}

char *memcached_result_value(memcached_result_st *ptr)
{
  memcached_string_st *sptr= &ptr->value;
  return memcached_string_value(sptr);
}

size_t memcached_result_length(memcached_result_st *ptr)
{
  memcached_string_st *sptr= &ptr->value;
  return memcached_string_length(sptr);
}

static memcached_return binary_read_one_response(memcached_server_st *ptr,
                                                 char *buffer, size_t buffer_length,
                                                 memcached_result_st *result)
{
  protocol_binary_response_header header;
   
  unlikely (memcached_safe_read(ptr, &header.bytes, 
                                sizeof(header.bytes)) != MEMCACHED_SUCCESS)
    return MEMCACHED_UNKNOWN_READ_FAILURE;

  unlikely (header.response.magic != PROTOCOL_BINARY_RES) 
    return MEMCACHED_PROTOCOL_ERROR;

  /*
  ** Convert the header to host local endian!
  */
  header.response.keylen= ntohs(header.response.keylen);
  header.response.status= ntohs(header.response.status);
  header.response.bodylen= ntohl(header.response.bodylen);
  header.response.cas= ntohll(header.response.cas);
  uint32_t bodylen= header.response.bodylen;

  if (header.response.status == 0) 
  {
    switch (header.response.opcode)
    {
    case PROTOCOL_BINARY_CMD_GETK:
    case PROTOCOL_BINARY_CMD_GETKQ:
      {
        uint16_t keylen= header.response.keylen;
        memcached_result_reset(result);
        result->cas= header.response.cas;

        if (memcached_safe_read(ptr, &result->flags,
                                sizeof (result->flags)) != MEMCACHED_SUCCESS)
          return MEMCACHED_UNKNOWN_READ_FAILURE;

        result->flags= ntohl(result->flags);
        bodylen -= header.response.extlen;

        result->key_length= keylen;
        if (memcached_safe_read(ptr, result->key, keylen) != MEMCACHED_SUCCESS) 
          return MEMCACHED_UNKNOWN_READ_FAILURE;

        bodylen -= keylen;
        if (memcached_string_check(&result->value,
                                   bodylen) != MEMCACHED_SUCCESS) 
          return MEMCACHED_MEMORY_ALLOCATION_FAILURE;

        char *vptr= memcached_string_value(&result->value);
        if (memcached_safe_read(ptr, vptr, bodylen) != MEMCACHED_SUCCESS) 
          return MEMCACHED_UNKNOWN_READ_FAILURE;

        memcached_string_set_length(&result->value, bodylen);  
      } 
      break;
    case PROTOCOL_BINARY_CMD_INCREMENT:
    case PROTOCOL_BINARY_CMD_DECREMENT:
      {
        if (bodylen != sizeof(uint64_t) || buffer_length != sizeof(uint64_t)) 
          return MEMCACHED_PROTOCOL_ERROR;

        WATCHPOINT_ASSERT(bodylen == buffer_length);
        uint64_t val;
        if (memcached_safe_read(ptr, &val, sizeof(val)) != MEMCACHED_SUCCESS) 
          return MEMCACHED_UNKNOWN_READ_FAILURE;

        val= ntohll(val);
        memcpy(buffer, &val, sizeof(val));
      } 
      break;
    case PROTOCOL_BINARY_CMD_VERSION:
      {
        memset(buffer, 0, buffer_length);
        if (bodylen >= buffer_length)
          /* not enough space in buffer.. should not happen... */
          return MEMCACHED_UNKNOWN_READ_FAILURE;
        else if (memcached_safe_read(ptr, buffer, bodylen) != MEMCACHED_SUCCESS)
          return MEMCACHED_UNKNOWN_READ_FAILURE;
      } 
      break;
    case PROTOCOL_BINARY_CMD_FLUSH:
    case PROTOCOL_BINARY_CMD_QUIT:
    case PROTOCOL_BINARY_CMD_SET:
    case PROTOCOL_BINARY_CMD_ADD:
    case PROTOCOL_BINARY_CMD_REPLACE:
    case PROTOCOL_BINARY_CMD_APPEND:
    case PROTOCOL_BINARY_CMD_PREPEND:
    case PROTOCOL_BINARY_CMD_DELETE:
      {
        WATCHPOINT_ASSERT(bodylen == 0);
        return MEMCACHED_SUCCESS;
      } 
      break;
    case PROTOCOL_BINARY_CMD_NOOP:
      {
        WATCHPOINT_ASSERT(bodylen == 0);
        return MEMCACHED_END;
      }
      break;
    case PROTOCOL_BINARY_CMD_STAT:
      {
        if (bodylen == 0)
          return MEMCACHED_END;
        else if (bodylen + 1 > buffer_length)
          /* not enough space in buffer.. should not happen... */
          return MEMCACHED_UNKNOWN_READ_FAILURE;
        else 
        {
          size_t keylen= header.response.keylen;            
          memset(buffer, 0, buffer_length);
          if (memcached_safe_read(ptr, buffer, keylen) != MEMCACHED_SUCCESS ||
              memcached_safe_read(ptr, buffer + keylen + 1, 
                                  bodylen - keylen) != MEMCACHED_SUCCESS)
            return MEMCACHED_UNKNOWN_READ_FAILURE;
        }
      } 
      break;
    default:
      {
        /* Command not implemented yet! */
        WATCHPOINT_ASSERT(0);
        return MEMCACHED_PROTOCOL_ERROR;
      }        
    }
  } 
  else if (header.response.bodylen) 
  {
     /* What should I do with the error message??? just discard it for now */
    char hole[SMALL_STRING_LEN];
    while (bodylen > 0) 
    {
      size_t nr= (bodylen > SMALL_STRING_LEN) ? SMALL_STRING_LEN : bodylen;
      if (memcached_safe_read(ptr, hole, nr) != MEMCACHED_SUCCESS)
        return MEMCACHED_UNKNOWN_READ_FAILURE;
      bodylen -= nr;
    }

    /* This might be an error from one of the quiet commands.. if
     * so, just throw it away and get the next one. What about creating
     * a callback to the user with the error information?
     */
    switch (header.response.opcode)
    {
    case PROTOCOL_BINARY_CMD_SETQ:
    case PROTOCOL_BINARY_CMD_ADDQ:
    case PROTOCOL_BINARY_CMD_REPLACEQ:
    case PROTOCOL_BINARY_CMD_APPENDQ:
    case PROTOCOL_BINARY_CMD_PREPENDQ:
      return binary_read_one_response(ptr, buffer, buffer_length, result);
    default:
      break;
    }
  }

  memcached_return rc= MEMCACHED_SUCCESS;
  unlikely(header.response.status != 0) 
    switch (header.response.status) 
    {
    case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
      rc= MEMCACHED_NOTFOUND;
      break;
    case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS:
      rc= MEMCACHED_DATA_EXISTS;
      break;
    case PROTOCOL_BINARY_RESPONSE_E2BIG:
    case PROTOCOL_BINARY_RESPONSE_EINVAL:
    case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
    case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
    case PROTOCOL_BINARY_RESPONSE_ENOMEM:
    default:
      /* @todo fix the error mappings */
      rc= MEMCACHED_PROTOCOL_ERROR;
      break;
    }
    
  return rc;
}
