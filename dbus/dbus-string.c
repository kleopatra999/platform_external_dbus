/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-string.c String utility class (internal to D-BUS implementation)
 * 
 * Copyright (C) 2002  Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 1.2
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-string.h"
/* we allow a system header here, for speed/convenience */
#include <string.h>

/**
 * @defgroup DBusString string class
 * @ingroup  DBusInternals
 * @brief DBusString data structure
 *
 * Types and functions related to DBusString. DBusString is intended
 * to be a string class that makes it hard to mess up security issues
 * (and just in general harder to write buggy code).  It should be
 * used (or extended and then used) rather than the libc stuff in
 * string.h.  The string class is a bit inconvenient at spots because
 * it handles out-of-memory failures and tries to be extra-robust.
 * 
 * A DBusString has a maximum length set at initialization time; this
 * can be used to ensure that a buffer doesn't get too big.  The
 * _dbus_string_lengthen() method checks for overflow, and for max
 * length being exceeded.
 * 
 * Try to avoid conversion to a plain C string, i.e. add methods on
 * the string object instead, only convert to C string when passing
 * things out to the public API. In particular, no sprintf, strcpy,
 * strcat, any of that should be used. The GString feature of
 * accepting negative numbers for "length of string" is also absent,
 * because it could keep us from detecting bogus huge lengths. i.e. if
 * we passed in some bogus huge length it would be taken to mean
 * "current length of string" instead of "broken crack"
 */

/**
 * @defgroup DBusStringInternals DBusString implementation details
 * @ingroup  DBusInternals
 * @brief DBusString implementation details
 *
 * The guts of DBusString.
 *
 * @{
 */

/**
 * @brief Internals of DBusString.
 * 
 * DBusString internals. DBusString is an opaque objects, it must be
 * used via accessor functions.
 */
typedef struct
{
  unsigned char *str;            /**< String data, plus nul termination */
  int            len;            /**< Length without nul */
  int            allocated;      /**< Allocated size of data */
  int            max_length;     /**< Max length of this string. */
  unsigned int   constant : 1;   /**< String data is not owned by DBusString */
  unsigned int   locked : 1;     /**< DBusString has been locked and can't be changed */
  unsigned int   invalid : 1;    /**< DBusString is invalid (e.g. already freed) */
} DBusRealString;

/**
 * Checks a bunch of assertions about a string object
 *
 * @param real the DBusRealString
 */
#define DBUS_GENERIC_STRING_PREAMBLE(real) _dbus_assert ((real) != NULL); _dbus_assert (!(real)->invalid); _dbus_assert ((real)->len >= 0); _dbus_assert ((real)->allocated >= 0); _dbus_assert ((real)->max_length >= 0); _dbus_assert ((real)->len <= (real)->allocated); _dbus_assert ((real)->len <= (real)->max_length)

/**
 * Checks assertions about a string object that needs to be
 * modifiable - may not be locked or const. Also declares
 * the "real" variable pointing to DBusRealString. 
 * @param str the string
 */
#define DBUS_STRING_PREAMBLE(str) DBusRealString *real = (DBusRealString*) str; \
  DBUS_GENERIC_STRING_PREAMBLE (real);                                          \
  _dbus_assert (!(real)->constant);                                             \
  _dbus_assert (!(real)->locked)

/**
 * Checks assertions about a string object that may be locked but
 * can't be const. i.e. a string object that we can free.  Also
 * declares the "real" variable pointing to DBusRealString.
 *
 * @param str the string
 */
#define DBUS_LOCKED_STRING_PREAMBLE(str) DBusRealString *real = (DBusRealString*) str; \
  DBUS_GENERIC_STRING_PREAMBLE (real);                                                 \
  _dbus_assert (!(real)->constant)

/**
 * Checks assertions about a string that may be const or locked.  Also
 * declares the "real" variable pointing to DBusRealString.
 * @param str the string.
 */
#define DBUS_CONST_STRING_PREAMBLE(str) const DBusRealString *real = (DBusRealString*) str; \
  DBUS_GENERIC_STRING_PREAMBLE (real)

/** @} */

/**
 * @addtogroup DBusString
 * @{
 */

/**
 * Initializes a string. The maximum length may be _DBUS_INT_MAX for
 * no maximum. The string starts life with zero length.
 * The string must eventually be freed with _dbus_string_free().
 *
 * @param str memory to hold the string
 * @param max_length the maximum size of the string
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_init (DBusString *str,
                   int         max_length)
{
  DBusRealString *real;
  
  _dbus_assert (str != NULL);
  _dbus_assert (max_length >= 0);

  _dbus_assert (sizeof (DBusString) == sizeof (DBusRealString));
  
  real = (DBusRealString*) str;

  /* It's very important not to touch anything
   * other than real->str if we're going to fail,
   * since we also use this function to reset
   * an existing string, e.g. in _dbus_string_steal_data()
   */
  
#define INITIAL_ALLOC 2
  
  real->str = dbus_malloc (INITIAL_ALLOC);
  if (real->str == NULL)
    return FALSE;

  real->allocated = INITIAL_ALLOC;
  real->len = 0;
  real->str[real->len] = '\0';
  
  real->max_length = max_length;
  real->constant = FALSE;
  real->locked = FALSE;
  real->invalid = FALSE;

  return TRUE;
}

/**
 * Initializes a constant string. The value parameter is not copied
 * (should be static), and the string may never be modified.
 * It is safe but not necessary to call _dbus_string_free()
 * on a const string.
 * 
 * @param str memory to use for the string
 * @param value a string to be stored in str (not copied!!!)
 */
void
_dbus_string_init_const (DBusString *str,
                         const char *value)
{
  DBusRealString *real;
  
  _dbus_assert (str != NULL);
  _dbus_assert (value != NULL);

  real = (DBusRealString*) str;
  
  real->str = (char*) value;
  real->len = strlen (real->str);
  real->allocated = real->len;
  real->max_length = real->len;
  real->constant = TRUE;
  real->invalid = FALSE;
}

/**
 * Frees a string created by _dbus_string_init().
 *
 * @param str memory where the string is stored.
 */
void
_dbus_string_free (DBusString *str)
{
  DBUS_LOCKED_STRING_PREAMBLE (str);
  
  if (real->constant)
    return;
  
  dbus_free (real->str);

  real->invalid = TRUE;
}

/**
 * Locks a string such that any attempts to change the string
 * will result in aborting the program. Also, if the string
 * is wasting a lot of memory (allocation is larger than what
 * the string is really using), _dbus_string_lock() will realloc
 * the string's data to "compact" it.
 *
 * @param str the string to lock.
 */
void
_dbus_string_lock (DBusString *str)
{  
  DBUS_LOCKED_STRING_PREAMBLE (str); /* can lock multiple times */

  real->locked = TRUE;

  /* Try to realloc to avoid excess memory usage, since
   * we know we won't change the string further
   */
#define MAX_WASTE 24
  if (real->allocated > (real->len + MAX_WASTE))
    {
      char *new_str;
      int new_allocated;

      new_allocated = real->len + 1;

      new_str = dbus_realloc (real->str, new_allocated);
      if (new_str != NULL)
        {
          real->str = new_str;
          real->allocated = new_allocated;
        }
    }
}

/**
 * Gets the raw character buffer from the string.  The returned buffer
 * will be nul-terminated, but note that strings may contain binary
 * data so there may be extra nul characters prior to the termination.
 * This function should be little-used, extend DBusString or add
 * stuff to dbus-sysdeps.c instead. It's an error to use this
 * function on a const string.
 *
 * @param str the string
 * @param data_return place to store the returned data
 */
void
_dbus_string_get_data (DBusString        *str,
                       char             **data_return)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  
  *data_return = real->str;
}

/**
 * Gets the raw character buffer from a const string. 
 *
 * @param str the string
 * @param data_return location to store returned data
 */
void
_dbus_string_get_const_data (const DBusString  *str,
                             const char       **data_return)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  
  *data_return = real->str;
}

/**
 * Gets a sub-portion of the raw character buffer from the
 * string. The "len" field is required simply for error
 * checking, to be sure you don't try to use more
 * string than exists. The nul termination of the
 * returned buffer remains at the end of the entire
 * string, not at start + len.
 *
 * @param str the string
 * @param data_return location to return the buffer
 * @param start byte offset to return
 * @param len length of segment to return
 */
void
_dbus_string_get_data_len (DBusString *str,
                           char      **data_return,
                           int         start,
                           int         len)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real->len);
  
  *data_return = real->str + start;
}

/**
 * const version of _dbus_string_get_data_len().
 *
 * @param str the string
 * @param data_return location to return the buffer
 * @param start byte offset to return
 * @param len length of segment to return
 */
void
_dbus_string_get_const_data_len (const DBusString  *str,
                                 const char       **data_return,
                                 int                start,
                                 int                len)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real->len);
  
  *data_return = real->str + start;
}

/**
 * Like _dbus_string_get_data(), but removes the
 * gotten data from the original string. The caller
 * must free the data returned. This function may
 * fail due to lack of memory, and return #FALSE.
 *
 * @param str the string
 * @param data_return location to return the buffer
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_steal_data (DBusString        *str,
                         char             **data_return)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  
  *data_return = real->str;

  /* reset the string */
  if (!_dbus_string_init (str, real->max_length))
    {
      /* hrm, put it back then */
      real->str = *data_return;
      *data_return = NULL;
      return FALSE;
    }

  return TRUE;
}

/**
 * Like _dbus_string_get_data_len(), but removes the gotten data from
 * the original string. The caller must free the data returned. This
 * function may fail due to lack of memory, and return #FALSE.
 * The returned string is nul-terminated and has length len.
 *
 * @param str the string
 * @param data_return location to return the buffer
 * @param start the start of segment to steal
 * @param len the length of segment to steal
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_steal_data_len (DBusString        *str,
                             char             **data_return,
                             int                start,
                             int                len)
{
  DBusString dest;
  
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (data_return != NULL);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real->len);

  if (!_dbus_string_init (&dest, real->max_length))
    return FALSE;

  if (!_dbus_string_move_len (str, start, len, &dest, 0))
    {
      _dbus_string_free (&dest);
      return FALSE;
    }
  
  if (!_dbus_string_steal_data (&dest, data_return))
    {
      _dbus_string_free (&dest);
      return FALSE;
    }

  _dbus_string_free (&dest);
  return TRUE;
}

/**
 * Gets the length of a string (not including nul termination).
 *
 * @returns the length.
 */
int
_dbus_string_get_length (const DBusString  *str)
{
  DBUS_CONST_STRING_PREAMBLE (str);
  
  return real->len;
}

static dbus_bool_t
set_length (DBusRealString *real,
            int             new_length)
{
  /* Note, we are setting the length without nul termination */

  /* exceeding max length is the same as failure to allocate memory */
  if (new_length > real->max_length)
    return FALSE;
  
  while (new_length >= real->allocated)
    {
      int new_allocated;
      char *new_str;
      
      new_allocated = 2 + real->allocated * 2;
      if (new_allocated < real->allocated)
        return FALSE; /* overflow */
        
      new_str = dbus_realloc (real->str, new_allocated);
      if (new_str == NULL)
        return FALSE;

      real->str = new_str;
      real->allocated = new_allocated;
    }

  real->len = new_length;
  real->str[real->len] = '\0';

  return TRUE;
}

/**
 * Makes a string longer by the given number of bytes.  Checks whether
 * adding additional_length to the current length would overflow an
 * integer, and checks for exceeding a string's max length.
 * The new bytes are not initialized, other than nul-terminating
 * the end of the string. The uninitialized bytes may contain
 * unexpected nul bytes or other junk.
 *
 * @param str a string
 * @param additional_length length to add to the string.
 * @returns #TRUE on success.
 */
dbus_bool_t
_dbus_string_lengthen (DBusString *str,
                       int         additional_length)
{
  DBUS_STRING_PREAMBLE (str);  
  _dbus_assert (additional_length >= 0);
  
  if ((real->len + additional_length) < real->len)
    return FALSE; /* overflow */
  
  return set_length (real,
                     real->len + additional_length);
}

/**
 * Makes a string shorter by the given number of bytes.
 *
 * @param str a string
 * @param length_to_remove length to remove from the string.
 */
void
_dbus_string_shorten (DBusString *str,
                      int         length_to_remove)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (length_to_remove >= 0);
  _dbus_assert (length_to_remove <= real->len);

  set_length (real,
              real->len - length_to_remove);
}

/**
 * Sets the length of a string. Can be used to truncate or lengthen
 * the string. If the string is lengthened, the function may fail and
 * return #FALSE. Newly-added bytes are not initialized, as with
 * _dbus_string_lengthen().
 *
 * @param str a string
 * @param length new length of the string.
 * @returns #FALSE on failure.
 */
dbus_bool_t
_dbus_string_set_length (DBusString *str,
                         int         length)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (length >= 0);

  return set_length (real, length);
}

static dbus_bool_t
append (DBusRealString *real,
        const char     *buffer,
        int             buffer_len)
{
  if (buffer_len == 0)
    return TRUE;

  if (!_dbus_string_lengthen ((DBusString*)real, buffer_len))
    return FALSE;

  memcpy (real->str + (real->len - buffer_len),
          buffer,
          buffer_len);

  return TRUE;
}

/**
 * Appends a nul-terminated C-style string to a DBusString.
 *
 * @param str the DBusString
 * @param buffer the nul-terminated characters to append
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_append (DBusString *str,
                     const char *buffer)
{
  int buffer_len;
  
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (buffer != NULL);
  
  buffer_len = strlen (buffer);

  return append (real, buffer, buffer_len);
}

/**
 * Appends block of bytes with the given length to a DBusString.
 *
 * @param str the DBusString
 * @param buffer the bytes to append
 * @param len the number of bytes to append
 * @returns #FALSE if not enough memory.
 */
dbus_bool_t
_dbus_string_append_len (DBusString *str,
                         const char *buffer,
                         int         len)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (buffer != NULL);
  _dbus_assert (len >= 0);

  return append (real, buffer, len);
}

/**
 * Appends a single byte to the string, returning #FALSE
 * if not enough memory.
 *
 * @param str the string
 * @param byte the byte to append
 * @returns #TRUE on success
 */
dbus_bool_t
_dbus_string_append_byte (DBusString    *str,
                          unsigned char  byte)
{
  DBUS_STRING_PREAMBLE (str);

  if (!set_length (real, real->len + 1))
    return FALSE;

  real->str[real->len-1] = byte;

  return TRUE;
}

/**
 * Appends a single Unicode character, encoding the character
 * in UTF-8 format.
 *
 * @param str the string
 * @param ch the Unicode character
 */
dbus_bool_t
_dbus_string_append_unichar (DBusString    *str,
                             dbus_unichar_t ch)
{
  int len;
  int first;
  int i;
  char *out;
  
  DBUS_STRING_PREAMBLE (str);

  /* this code is from GLib but is pretty standard I think */
  
  len = 0;
  
  if (ch < 0x80)
    {
      first = 0;
      len = 1;
    }
  else if (ch < 0x800)
    {
      first = 0xc0;
      len = 2;
    }
  else if (ch < 0x10000)
    {
      first = 0xe0;
      len = 3;
    }
   else if (ch < 0x200000)
    {
      first = 0xf0;
      len = 4;
    }
  else if (ch < 0x4000000)
    {
      first = 0xf8;
      len = 5;
    }
  else
    {
      first = 0xfc;
      len = 6;
    }

  if (!set_length (real, real->len + len))
    return FALSE;

  out = real->str + (real->len - len);
  
  for (i = len - 1; i > 0; --i)
    {
      out[i] = (ch & 0x3f) | 0x80;
      ch >>= 6;
    }
  out[0] = ch | first;

  return TRUE;
}

static void
delete (DBusRealString *real,
        int             start,
        int             len)
{
  if (len == 0)
    return;
  
  memmove (real->str + start, real->str + start + len, real->len - (start + len));
  real->len -= len;
  real->str[real->len] = '\0';
}

/**
 * Deletes a segment of a DBusString with length len starting at
 * start. (Hint: to clear an entire string, setting length to 0
 * with _dbus_string_set_length() is easier.)
 *
 * @param str the DBusString
 * @param start where to start deleting
 * @param len the number of bytes to delete
 */
void
_dbus_string_delete (DBusString       *str,
                     int               start,
                     int               len)
{
  DBUS_STRING_PREAMBLE (str);
  _dbus_assert (start >= 0);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real->len);

  delete (real, start, len);
}

static dbus_bool_t
copy (DBusRealString *source,
      int             start,
      int             len,
      DBusRealString *dest,
      int             insert_at)
{
  if (len == 0)
    return TRUE;

  if (!set_length (dest, dest->len + len))
    return FALSE;

  memmove (dest->str + insert_at + len, 
           dest->str + insert_at,
           dest->len - len);

  memcpy (dest->str + insert_at,
          source->str + start,
          len);

  return TRUE;
}

/**
 * Checks assertions for two strings we're copying a segment between,
 * and declares real_source/real_dest variables.
 *
 * @param source the source string
 * @param start the starting offset
 * @param dest the dest string
 * @param insert_at where the copied segment is inserted
 */
#define DBUS_STRING_COPY_PREAMBLE(source, start, dest, insert_at)       \
  DBusRealString *real_source = (DBusRealString*) source;               \
  DBusRealString *real_dest = (DBusRealString*) dest;                   \
  _dbus_assert ((source) != (dest));                                    \
  DBUS_GENERIC_STRING_PREAMBLE (real_source);                           \
  DBUS_GENERIC_STRING_PREAMBLE (real_dest);                             \
  _dbus_assert (!real_source->constant);                                \
  _dbus_assert (!real_source->locked);                                  \
  _dbus_assert (!real_dest->constant);                                  \
  _dbus_assert (!real_dest->locked);                                    \
  _dbus_assert ((start) >= 0);                                          \
  _dbus_assert ((start) <= real_source->len);                           \
  _dbus_assert ((insert_at) >= 0);                                      \
  _dbus_assert ((insert_at) <= real_dest->len)

/**
 * Moves the end of one string into another string. Both strings
 * must be initialized, valid strings.
 *
 * @param source the source string
 * @param start where to chop off the source string
 * @param dest the destination string
 * @param insert_at where to move the chopped-off part of source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_move (DBusString       *source,
                   int               start,
                   DBusString       *dest,
                   int               insert_at)
{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);
  
  if (!copy (real_source, start,
             real_source->len - start,
             real_dest,
             insert_at))
    return FALSE;

  delete (real_source, start,
          real_source->len - start);

  return TRUE;
}

/**
 * Like _dbus_string_move(), but does not delete the section
 * of the source string that's copied to the dest string.
 *
 * @param source the source string
 * @param start where to start copying the source string
 * @param dest the destination string
 * @param insert_at where to place the copied part of source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_copy (const DBusString *source,
                   int               start,
                   DBusString       *dest,
                   int               insert_at)
{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);

  return copy (real_source, start,
               real_source->len - start,
               real_dest,
               insert_at);
}

/**
 * Like _dbus_string_move(), but can move a segment from
 * the middle of the source string.
 * 
 * @param source the source string
 * @param start first byte of source string to move
 * @param len length of segment to move
 * @param dest the destination string
 * @param insert_at where to move the bytes from the source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_move_len (DBusString       *source,
                       int               start,
                       int               len,
                       DBusString       *dest,
                       int               insert_at)

{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real_source->len);

  if (!copy (real_source, start, len,
             real_dest,
             insert_at))
    return FALSE;

  delete (real_source, start,
          real_source->len - start);

  return TRUE;
}

/**
 * Like _dbus_string_copy(), but can copy a segment from the middle of
 * the source string.
 *
 * @param source the source string
 * @param start where to start copying the source string
 * @param len length of segment to copy
 * @param dest the destination string
 * @param insert_at where to place the copied segment of source string
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
_dbus_string_copy_len (const DBusString *source,
                       int               start,
                       int               len,
                       DBusString       *dest,
                       int               insert_at)
{
  DBUS_STRING_COPY_PREAMBLE (source, start, dest, insert_at);
  _dbus_assert (len >= 0);
  _dbus_assert ((start + len) <= real_source->len);
  
  return copy (real_source, start, len,
               real_dest,
               insert_at);
}

/* Unicode macros from GLib */

/** computes length and mask of a unicode character
 * @param Char the char
 * @param Mask the mask variable to assign to
 * @param Len the length variable to assign to
 */
#define UTF8_COMPUTE(Char, Mask, Len)					      \
  if (Char < 128)							      \
    {									      \
      Len = 1;								      \
      Mask = 0x7f;							      \
    }									      \
  else if ((Char & 0xe0) == 0xc0)					      \
    {									      \
      Len = 2;								      \
      Mask = 0x1f;							      \
    }									      \
  else if ((Char & 0xf0) == 0xe0)					      \
    {									      \
      Len = 3;								      \
      Mask = 0x0f;							      \
    }									      \
  else if ((Char & 0xf8) == 0xf0)					      \
    {									      \
      Len = 4;								      \
      Mask = 0x07;							      \
    }									      \
  else if ((Char & 0xfc) == 0xf8)					      \
    {									      \
      Len = 5;								      \
      Mask = 0x03;							      \
    }									      \
  else if ((Char & 0xfe) == 0xfc)					      \
    {									      \
      Len = 6;								      \
      Mask = 0x01;							      \
    }									      \
  else									      \
    Len = -1;

/**
 * computes length of a unicode character in UTF-8
 * @param Char the char
 */
#define UTF8_LENGTH(Char)              \
  ((Char) < 0x80 ? 1 :                 \
   ((Char) < 0x800 ? 2 :               \
    ((Char) < 0x10000 ? 3 :            \
     ((Char) < 0x200000 ? 4 :          \
      ((Char) < 0x4000000 ? 5 : 6)))))
   
/**
 * Gets a UTF-8 value.
 *
 * @param Result variable for extracted unicode char.
 * @param Chars the bytes to decode
 * @param Count counter variable
 * @param Mask mask for this char
 * @param Len length for this char in bytes
 */
#define UTF8_GET(Result, Chars, Count, Mask, Len)			      \
  (Result) = (Chars)[0] & (Mask);					      \
  for ((Count) = 1; (Count) < (Len); ++(Count))				      \
    {									      \
      if (((Chars)[(Count)] & 0xc0) != 0x80)				      \
	{								      \
	  (Result) = -1;						      \
	  break;							      \
	}								      \
      (Result) <<= 6;							      \
      (Result) |= ((Chars)[(Count)] & 0x3f);				      \
    }

/**
 * Check whether a unicode char is in a valid range.
 *
 * @param Char the character
 */
#define UNICODE_VALID(Char)                   \
    ((Char) < 0x110000 &&                     \
     ((Char) < 0xD800 || (Char) >= 0xE000) && \
     (Char) != 0xFFFE && (Char) != 0xFFFF)   

/**
 * Gets a unicode character from a UTF-8 string. Does no validation;
 * you must verify that the string is valid UTF-8 in advance and must
 * pass in the start of a character.
 *
 * @param str the string
 * @param start the start of the UTF-8 character.
 * @param ch_return location to return the character
 * @param end_return location to return the byte index of next character
 * @returns #TRUE on success, #FALSE otherwise.
 */
void
_dbus_string_get_unichar (const DBusString *str,
                          int               start,
                          dbus_unichar_t   *ch_return,
                          int              *end_return)
{
  int i, mask, len;
  dbus_unichar_t result;
  unsigned char c;
  unsigned char *p;
  DBUS_CONST_STRING_PREAMBLE (str);

  if (ch_return)
    *ch_return = 0;
  if (end_return)
    *end_return = real->len;
  
  mask = 0;
  p = real->str + start;
  c = *p;
  
  UTF8_COMPUTE (c, mask, len);
  if (len == -1)
    return;
  UTF8_GET (result, p, i, mask, len);

  if (result == (dbus_unichar_t)-1)
    return;

  if (ch_return)
    *ch_return = result;
  if (end_return)
    *end_return = start + len;
}

/** @} */

#ifdef DBUS_BUILD_TESTS
#include "dbus-test.h"
#include <stdio.h>

static void
test_max_len (DBusString *str,
              int         max_len)
{
  if (max_len > 0)
    {
      if (!_dbus_string_set_length (str, max_len - 1))
        _dbus_assert_not_reached ("setting len to one less than max should have worked");
    }

  if (!_dbus_string_set_length (str, max_len))
    _dbus_assert_not_reached ("setting len to max len should have worked");

  if (_dbus_string_set_length (str, max_len + 1))
    _dbus_assert_not_reached ("setting len to one more than max len should not have worked");

  if (!_dbus_string_set_length (str, 0))
    _dbus_assert_not_reached ("setting len to zero should have worked");
}

/**
 * @ingroup DBusStringInternals
 * Unit test for DBusString.
 *
 * @todo Need to write tests for _dbus_string_copy() and
 * _dbus_string_move() moving to/from each of start/middle/end of a
 * string.
 * 
 * @returns #TRUE on success.
 */
dbus_bool_t
_dbus_string_test (void)
{
  DBusString str;
  DBusString other;
  int i, end;
  long v;
  double d;
  int lens[] = { 0, 1, 2, 3, 4, 5, 10, 16, 17, 18, 25, 31, 32, 33, 34, 35, 63, 64, 65, 66, 67, 68, 69, 70, 71, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136 };
  char *s;
  dbus_unichar_t ch;
  
  i = 0;
  while (i < _DBUS_N_ELEMENTS (lens))
    {
      if (!_dbus_string_init (&str, lens[i]))
        _dbus_assert_not_reached ("failed to init string");
      
      test_max_len (&str, lens[i]);
      _dbus_string_free (&str);

      ++i;
    }

  /* Test shortening and setting length */
  i = 0;
  while (i < _DBUS_N_ELEMENTS (lens))
    {
      int j;
      
      if (!_dbus_string_init (&str, lens[i]))
        _dbus_assert_not_reached ("failed to init string");
      
      if (!_dbus_string_set_length (&str, lens[i]))
        _dbus_assert_not_reached ("failed to set string length");

      j = lens[i];
      while (j > 0)
        {
          _dbus_assert (_dbus_string_get_length (&str) == j);
          if (j > 0)
            {
              _dbus_string_shorten (&str, 1);
              _dbus_assert (_dbus_string_get_length (&str) == (j - 1));
            }
          --j;
        }
      
      _dbus_string_free (&str);

      ++i;
    }

  /* Test appending data */
  if (!_dbus_string_init (&str, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("failed to init string");

  i = 0;
  while (i < 10)
    {
      if (!_dbus_string_append (&str, "a"))
        _dbus_assert_not_reached ("failed to append string to string\n");

      _dbus_assert (_dbus_string_get_length (&str) == i * 2 + 1);

      if (!_dbus_string_append_byte (&str, 'b'))
        _dbus_assert_not_reached ("failed to append byte to string\n");

      _dbus_assert (_dbus_string_get_length (&str) == i * 2 + 2);
                    
      ++i;
    }

  _dbus_string_free (&str);

  /* Check steal_data */
  
  if (!_dbus_string_init (&str, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("failed to init string");

  if (!_dbus_string_append (&str, "Hello World"))
    _dbus_assert_not_reached ("could not append to string");

  i = _dbus_string_get_length (&str);
  
  if (!_dbus_string_steal_data (&str, &s))
    _dbus_assert_not_reached ("failed to steal data");

  _dbus_assert (_dbus_string_get_length (&str) == 0);
  _dbus_assert (((int)strlen (s)) == i);

  dbus_free (s);

  /* Check move */
  
  if (!_dbus_string_append (&str, "Hello World"))
    _dbus_assert_not_reached ("could not append to string");

  i = _dbus_string_get_length (&str);

  if (!_dbus_string_init (&other, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("could not init string");
  
  if (!_dbus_string_move (&str, 0, &other, 0))
    _dbus_assert_not_reached ("could not move");

  _dbus_assert (_dbus_string_get_length (&str) == 0);
  _dbus_assert (_dbus_string_get_length (&other) == i);

  _dbus_string_free (&other);

  /* Check copy */
  
  if (!_dbus_string_append (&str, "Hello World"))
    _dbus_assert_not_reached ("could not append to string");

  i = _dbus_string_get_length (&str);
  
  if (!_dbus_string_init (&other, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("could not init string");
  
  if (!_dbus_string_copy (&str, 0, &other, 0))
    _dbus_assert_not_reached ("could not copy");

  _dbus_assert (_dbus_string_get_length (&str) == i);
  _dbus_assert (_dbus_string_get_length (&other) == i);
  
  _dbus_string_free (&str);
  _dbus_string_free (&other);

  /* Check append/get unichar */
  
  if (!_dbus_string_init (&str, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("failed to init string");

  ch = 0;
  if (!_dbus_string_append_unichar (&str, 0xfffc))
    _dbus_assert_not_reached ("failed to append unichar");

  _dbus_string_get_unichar (&str, 0, &ch, &i);

  _dbus_assert (ch == 0xfffc);
  _dbus_assert (i == _dbus_string_get_length (&str));

  _dbus_string_free (&str);
  
  /* Check append/parse int/double */
  
  if (!_dbus_string_init (&str, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("failed to init string");

  if (!_dbus_string_append_int (&str, 27))
    _dbus_assert_not_reached ("failed to append int");

  i = _dbus_string_get_length (&str);

  if (!_dbus_string_parse_int (&str, 0, &v, &end))
    _dbus_assert_not_reached ("failed to parse int");

  _dbus_assert (v == 27);
  _dbus_assert (end == i);
    
  _dbus_string_set_length (&str, 0);

  if (!_dbus_string_init (&str, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("failed to init string");
  
  if (!_dbus_string_append_double (&str, 50.3))
    _dbus_assert_not_reached ("failed to append float");

  i = _dbus_string_get_length (&str);

  if (!_dbus_string_parse_double (&str, 0, &d, &end))
    _dbus_assert_not_reached ("failed to parse float");

  _dbus_assert (d > (50.3 - 1e-6) && d < (50.3 + 1e-6));
  _dbus_assert (end == i);

  _dbus_string_free (&str);

  return TRUE;
}

#endif /* DBUS_BUILD_TESTS */
