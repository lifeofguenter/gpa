/* gpapa.c - The GNU Privacy Assistant Pipe Access
 * Copyright (C) 2000, 2001 G-N-U GmbH, http://www.g-n-u.de
 *
 * This file is part of GPAPA.
 *
 * GPAPA is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GPAPA is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GPAPA; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "gpapa.h"

#ifdef __USE_HKP__

#include <keyserver.h>

#define KEY_BUFLEN 65536

gchar *hkp_errtypestr[] =
  {
    N_("General error"),
    N_("The keyserver returned an error message:\n\n%s"),
    N_("The keyserver returned an error message:\n\n%s"),
    N_("Keyserver timeout"),
    N_("Error initializing network"),
    N_("Error resolving host name"),
    N_("Socket error"),
    N_("Error while connecting to keyserver")
  };

#endif /* __USE_HKP__ */

#ifdef _WIN32
#include <windows.h>
#endif

char *global_keyServer;

static char *gpg_program;


/* Key management.
 */

/* The public and secret key ring. We only allow for one of each.
 */
static GList *PubRing = NULL, *SecRing = NULL;
static gboolean pubring_initialized = FALSE, secring_initialized = FALSE;

/* Release a public key ring G. This is the only place where
 * gpapa_public_key_release() is called.
 */
static void
release_public_keyring (GList *g)
{
  while (g)
    {
      gpapa_public_key_release (g->data);
      g = g_list_next (g);
    }
  g_list_free (g);
}

/* Release a secret key ring G. This is the only place where
 * gpapa_secret_key_release() is called.
 */
static void
release_secret_keyring (GList *g)
{
  while (g)
    {
      gpapa_secret_key_release (g->data);
      g = g_list_next (g);
    }
  g_list_free (g);
}

/* Sort public keys alphabetically.
 */
static int
compare_public_keys (gconstpointer a, gconstpointer b)
{
  const GpapaPublicKey *k1 = a, *k2 = b;
  if (k1->key && k2->key && k1->key->UserID && k2->key->UserID)
    return (strcasecmp (k1->key->UserID, k2->key->UserID));
  else
    return (0);
}

/* Sort secret keys alphabetically.
 */
static int
compare_secret_keys (gconstpointer a, gconstpointer b)
{
  const GpapaPublicKey *k1 = a, *k2 = b;
  if (k1->key && k2->key && k1->key->UserID && k2->key->UserID)
    return (strcasecmp (k1->key->UserID, k2->key->UserID));
  else
    return (0);
}

/* Extract a key fingerprint out of the string BUFFER and return
 * a newly allocated buffer with the fingerprint according to
 * the algorithm ALGORITHM.
 */
char *
gpapa_extract_fingerprint (char *line, int algorithm,
                           GpapaCallbackFunc callback, gpointer calldata)
{
  char *field[GPAPA_MAX_GPG_KEY_FIELDS];
  char *p = line;
  gint i = 0;
  gint fields;

  while (*p)
    {
      field[i] = p;
      while (*p && *p != ':')
        p++;
      if (*p == ':')
        {
          *p = 0;
          p++;
        }
      i++;
      if (i >= GPAPA_MAX_GPG_KEY_FIELDS)
          break;
    }
  fields = i;
  if (fields < 10)
    {
      callback (GPAPA_ACTION_ERROR,
                _("Invalid number of fields in GnuPG colon output"), calldata);
      return (NULL);
    }
  else
    {
      if (algorithm == 1)  /* RSA */
        {
          char *fpraw = field[9];
          char *fp = xmalloc (strlen (fpraw) + 16 + 1);
          char *r = fpraw, *q = fp;
          gint c = 0;
          while (*r)
            {
              *q++ = *r++;
              c++;
              if (c < 32)
                {
                  if (c % 2 == 0)
                    *q++ = ' ';
                  if (c % 16 == 0)
                    *q++ = ' ';
                }
            }
          *q = 0;
          return fp;
        }
      else
        {
          char *fpraw = field[9];
          char *fp = xmalloc (strlen (fpraw) + 10 + 1);
          char *r = fpraw, *q = fp;
          gint c = 0;
          while (*r)
            {
              *q++ = *r++;
              c++;
              if (c < 40)
                {
                  if (c % 4 == 0)
                    *q++ = ' ';
                  if (c % 20 == 0)
                    *q++ = ' ';
                }
            }
          *q = 0;
          return fp;
        }
    }
}

/* Extract a string BUFFER like "2001-05-31" to a GDate structure;
 * return NULL on error.
 */
GDate *
gpapa_extract_date (char *buffer)
{
  char *p, *year, *month, *day;
  if (buffer == NULL)
    return (NULL);
  p = buffer;
  year = p;
  while (*p >= '0' && *p <= '9')
    p++;
  if (*p)
    {
      *p = 0;
      p++;
    }
  month = p;
  while (*p >= '0' && *p <= '9')
    p++;
  if (*p)
    {
      *p = 0;
      p++;
    }
  day = p;
  if (year && *year && month && *month && day && *day)
    return (g_date_new_dmy (atoi (day), atoi (month), atoi (year)));
  else
    return (NULL);
}

/* Extract a line LINE of gpg's colon output to a GpapaKey;
 * return NULL on error.
 */
static GpapaKey *
extract_key (char *line, GpapaCallbackFunc callback, gpointer calldata)
{
  char *field[GPAPA_MAX_GPG_KEY_FIELDS];
  char *p = line;
  gint i = 0;
  gint fields;

  while (*p)
    {
      field[i] = p;
      while (*p && *p != ':')
        p++;
      if (*p == ':')
        {
          *p = 0;
          p++;
        }
      i++;
      if (i >= GPAPA_MAX_GPG_KEY_FIELDS)
          break;
    }
  fields = i;
  if (fields < 10)
    {
      callback (GPAPA_ACTION_ERROR,
                _("Invalid number of fields in GnuPG colon output"), calldata);
      return (NULL);
    }
  else
    {
      char *quoted;
      GpapaKey *key = gpapa_key_new (field[7], callback, calldata);
      key->KeyTrust = field[1][0];
      key->bits = atoi (field[2]);
      key->algorithm = atoi (field[3]);
      if (field[4] == NULL || strlen (field[4]) <= 8)
        key->KeyID = NULL;
      else 
        key->KeyID = xstrdup (field[4] + 8);
      key->CreationDate = gpapa_extract_date (field[5]);
      key->ExpirationDate = gpapa_extract_date (field[6]);
      key->OwnerTrust = field[8][0];
      key->UserID = xstrdup (field[9]);

      /* Special case (really?): a quoted colon.
       * Un-quote it manually.
       */
      while ((quoted = strstr (key->UserID, "\\x3a")) != NULL)
        {
          quoted[0] = ':';
          quoted++;
          while (quoted[3])
            {
              quoted[0] = quoted[3];
              quoted++;
            }
          quoted[0] = 0;
        }
      return (key);
    }
}

static void
linecallback_refresh_pub (char *line, gpointer data, GpgStatusCode status)
{
  static GpapaPublicKey *key = NULL;
  PublicKeyData *d = data;
  gpapa_report_error_status (status, d->callback, d->calldata);
  if (status == NO_STATUS && line)
    {
      if (strncmp (line, "pub:", 4) == 0)
        {
#ifdef DEBUG
          fprintf (stderr, "extracting key: %s\n", line);
          fflush (stderr);
#endif
          key = (GpapaPublicKey *) xmalloc (sizeof (GpapaPublicKey));
          memset (key, 0, sizeof (GpapaPublicKey));
          key->key = extract_key (line, d->callback, d->calldata);
          PubRing = g_list_append (PubRing, key);
        }
      else if (strncmp (line, "fpr:", 4) == 0 && key && key->key)
        {
#ifdef DEBUG
          fprintf (stderr, "extracting fingerprint: %s\n", line);
          fflush (stderr);
#endif
          key->fingerprint = gpapa_extract_fingerprint (line, key->key->algorithm,
                                                        d->callback, d->calldata);
        }
    }
}

void
gpapa_refresh_public_keyring (GpapaCallbackFunc callback, gpointer calldata)
{
  PublicKeyData data = { NULL, callback, calldata };
  const gchar *gpgargv[4];
  pubring_initialized = TRUE;
  if (SecRing)
    {
      release_secret_keyring (SecRing);
      SecRing = NULL;
    }
  if (PubRing)
    {
      release_public_keyring (PubRing);
      PubRing = NULL;
    }
  gpgargv[0] = "--list-keys";
  gpgargv[1] = "--with-colons";
  gpgargv[2] = "--with-fingerprint";
  gpgargv[3] = NULL;
  gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
		    linecallback_refresh_pub, &data, callback, calldata);
  PubRing = g_list_sort (PubRing, compare_public_keys);
  gpapa_refresh_secret_keyring (callback, calldata);
}

gint
gpapa_get_public_key_count (GpapaCallbackFunc callback, gpointer calldata)
{
  if (! pubring_initialized)
    gpapa_refresh_public_keyring (callback, calldata);
  return (g_list_length (PubRing));
}

GpapaPublicKey *
gpapa_get_public_key_by_index (gint idx, GpapaCallbackFunc callback,
			       gpointer calldata)
{
  if (! pubring_initialized)
    gpapa_refresh_public_keyring (callback, calldata);
  return (g_list_nth_data (PubRing, idx));
}

GpapaPublicKey *
gpapa_get_public_key_by_ID (const gchar *keyID, GpapaCallbackFunc callback,
			    gpointer calldata)
{
  GList *g;
  GpapaPublicKey *p = NULL;
  if (! pubring_initialized)
    gpapa_refresh_public_keyring (callback, calldata);
  g = PubRing;
  while (g && g->data
           && (p = (GpapaPublicKey *) g->data) != NULL
           && p->key
           && p->key->KeyID
           && strcmp (p->key->KeyID, keyID) != 0)
    g = g_list_next (g);
  if (g && p)
    return p;
  else
    return NULL;
}

static void
linecallback_id_pub (char *line, gpointer data, GpgStatusCode status)
{
  PublicKeyData *d = data;
  gpapa_report_error_status (status, d->callback, d->calldata);
  if (status == NO_STATUS && line && strncmp (line, "pub:", 4) == 0)
    {
      d->key = (GpapaPublicKey *) xmalloc (sizeof (GpapaPublicKey));
      memset (d->key, 0, sizeof (GpapaPublicKey));
      d->key->key = extract_key (line, d->callback, d->calldata);
    }
}

GpapaPublicKey *
gpapa_get_public_key_by_userID (const gchar *userID, GpapaCallbackFunc callback,
			        gpointer calldata)
{
  PublicKeyData data = { NULL, callback, calldata };
  const gchar *gpgargv[4];
  char *uid = xstrdup (userID);
  uid = strcpy (uid, userID);
  gpgargv[0] = "--list-keys";
  gpgargv[1] = "--with-colons";
  gpgargv[2] = uid;
  gpgargv[3] = NULL;
  gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
		    linecallback_id_pub, &data, callback, calldata);
  free (uid);
  if (data.key)
    {
      GpapaPublicKey *result;
      if (data.key->key)
        result = gpapa_get_public_key_by_ID (data.key->key->KeyID,
                                             callback, calldata);
      else
        result = NULL;
      gpapa_public_key_release (data.key);
      return result;
    }
  else
    return NULL;
}

#ifdef __USE_HKP__

static gchar *
gpapa_http_quote (const gchar *s)
{
  gchar *quoted, *q;
  const gchar *p, hexchr[] = "0123456789abcdef";

  quoted = xmalloc (3 * strlen (s) + 1);
  q = quoted;
  for (p = s; *p; p++)
    {
      if ((*p >= 'A' && *p <= 'Z')
          || (*p >= 'a' && *p <= 'z')
          || (*p >= '0' && *p <= '9'))
        *q++ = *p;
      else
        {
          *q++ = '%';
          *q++ = hexchr[*p / 16];
          *q++ = hexchr[*p % 16];
        }
    }
  *q = 0;
  return quoted;
}

/* Remove HTML tags.
 */
static gchar *
gpapa_dehtml (const gchar *p)
{
  gchar *result = xmalloc (strlen (p));
  gchar *q = result;
  while (*p)
    {
      if (*p == '<')
        {
          while (*p && *p != '>')
            p++;
          p++;
        }
      else
        *q++ = *p++;
    }
  *q = 0;
  return result;
}

void
gpapa_report_hkp_error (int rc, GpapaCallbackFunc callback, gpointer calldata)
{
  if (rc < 1 && rc > 8)
    rc = HKPERR_GENERAL;
  if (rc == HKPERR_RECVKEY || rc == HKPERR_SENDKEY)
    {
      gchar *server_errmsg, *errmsg;
      server_errmsg = gpapa_dehtml (kserver_strerror ());
      errmsg = g_strdup_printf (_(hkp_errtypestr[rc - 1]),
                                server_errmsg);
      callback (GPAPA_ACTION_ERROR, errmsg, calldata);
      g_free (server_errmsg);
      g_free (errmsg);
    }
  else
    callback (GPAPA_ACTION_ERROR, _(hkp_errtypestr[rc - 1]), calldata);
}

#endif /* __USE_HKP__ */

GpapaPublicKey *
gpapa_receive_public_key_from_server (const gchar *keyID,
                                      const gchar *ServerName,
				      GpapaCallbackFunc callback,
				      gpointer calldata)
{
  if (keyID && ServerName)
    {
#ifdef __USE_HKP__
      gchar *key_buffer = g_malloc (KEY_BUFLEN);
      gchar *quoted_keyID = gpapa_http_quote (keyID);
      int rc;
      wsock_init ();
      rc = kserver_recvkey (ServerName, quoted_keyID, key_buffer, KEY_BUFLEN);
      free (quoted_keyID);
      wsock_end ();
      if (rc != 0)
        gpapa_report_hkp_error (rc, callback, calldata);
      else
	{
          const gchar *gpgargv[2];
          gpgargv[0] = "--import";
          gpgargv[1] = NULL;
          gpapa_call_gnupg (gpgargv, TRUE, key_buffer, NULL, NULL,
			    NULL, NULL, callback, calldata);
          g_free (key_buffer);
	}
#else /* not __USE_HKP__ */
      const gchar *gpgargv[5];
      char *id = xstrcat2 ("0x", keyID);
      gpgargv[0] = "--keyserver";
      gpgargv[1] = ServerName;
      gpgargv[2] = "--recv-keys";
      gpgargv[3] = id;
      gpgargv[4] = NULL;
      gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
			NULL, NULL, callback, calldata);
      free (id);
#endif /* not __USE_HKP__ */
      gpapa_refresh_public_keyring (callback, calldata);
    }
  return (gpapa_get_public_key_by_userID (keyID, callback, calldata));
}

#ifdef __USE_HKP__

GList *
gpapa_search_public_keys_on_server (const gchar *keyID, const gchar *ServerName,
				    GpapaCallbackFunc callback,
				    gpointer calldata)
{
  int rc, conn_fd;
  keyserver_key key_buffer;
  GList *g = NULL;
  gchar *quoted_keyID;

  if (!ServerName)
    return NULL;
  wsock_init ();
  quoted_keyID = gpapa_http_quote (keyID);
  rc = kserver_search_init (ServerName, quoted_keyID, &conn_fd);
  free (quoted_keyID);
  if (rc != 0)
    {
      gpapa_report_hkp_error (rc, callback, calldata);
      wsock_end ();
      return NULL;
    }
  while ((rc = kserver_search (conn_fd, &key_buffer)) == 0)
    {
      if (key_buffer.keyid[0])
        {
          GpapaKey *key = gpapa_key_new (key_buffer.keyid, callback, calldata);
          key->UserID = xstrdup (key_buffer.uid);
          g = g_list_append (g, key);
        }
    }
  wsock_end ();
  if (rc != 1)
    {
      gpapa_report_hkp_error (rc, callback, calldata);
    }
  return g;
}

#endif /* __USE_HKP__ */

static void
linecallback_refresh_sec (char *line, gpointer data, GpgStatusCode status)
{
  SecretKeyData *d = data;
  gpapa_report_error_status (status, d->callback, d->calldata);
  if (status == NO_STATUS && line && strncmp (line, "sec", 3) == 0)
    {
      GpapaSecretKey *key =
	(GpapaSecretKey *) xmalloc (sizeof (GpapaSecretKey));
      memset (key, 0, sizeof (GpapaSecretKey));
      key->key = extract_key (line, d->callback, d->calldata);
      if (key->key && key->key->KeyID && PubRing)
        {
          GpapaPublicKey *pubkey
            = gpapa_get_public_key_by_ID (key->key->KeyID,
                                          d->callback, d->calldata);
          if (pubkey)
            {
              gpapa_key_release (key->key);
              key->key = pubkey->key;
            }
        }
      SecRing = g_list_append (SecRing, key);
    }
}

void
gpapa_refresh_secret_keyring (GpapaCallbackFunc callback, gpointer calldata)
{
  SecretKeyData data = { NULL, callback, calldata };
  const gchar *gpgargv[3];
  secring_initialized = TRUE;
  if (SecRing != NULL)
    {
      g_list_free (SecRing);
      SecRing = NULL;
    }
  gpgargv[0] = "--list-secret-keys";
  gpgargv[1] = "--with-colons";
  gpgargv[2] = NULL;
  gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
		    linecallback_refresh_sec, &data, callback, calldata);
  SecRing = g_list_sort (SecRing, compare_secret_keys);
}

gint
gpapa_get_secret_key_count (GpapaCallbackFunc callback, gpointer calldata)
{
  if (! secring_initialized)
    gpapa_refresh_secret_keyring (callback, calldata);
  return (g_list_length (SecRing));
}

GpapaSecretKey *
gpapa_get_secret_key_by_index (gint idx, GpapaCallbackFunc callback,
			       gpointer calldata)
{
  if (! secring_initialized)
    gpapa_refresh_secret_keyring (callback, calldata);
  return (g_list_nth_data (SecRing, idx));
}

GpapaSecretKey *
gpapa_get_secret_key_by_ID (const gchar *keyID, GpapaCallbackFunc callback,
			    gpointer calldata)
{
  GList *g;
  GpapaSecretKey *s = NULL;
  if (! secring_initialized)
    gpapa_refresh_secret_keyring (callback, calldata);
  g = SecRing;
  while (g && g->data
           && (s = (GpapaSecretKey *) g->data) != NULL
           && s->key
           && s->key->KeyID
           && strcmp (s->key->KeyID, keyID) != 0)
    g = g_list_next (g);
  if (g && s)
    return s;
  else
    return NULL;
}
 
static void
linecallback_id_sec (char *line, gpointer data, GpgStatusCode status)
{
  SecretKeyData *d = data;
  gpapa_report_error_status (status, d->callback, d->calldata);
  if (status == NO_STATUS && line && strncmp (line, "sec", 3) == 0)
    {
      d->key = (GpapaSecretKey *) xmalloc (sizeof (GpapaSecretKey));
      memset (d->key, 0, sizeof (GpapaSecretKey));
      d->key->key = extract_key (line, d->callback, d->calldata);
    }
}

GpapaSecretKey *
gpapa_get_secret_key_by_userID (const gchar *userID, GpapaCallbackFunc callback,
			        gpointer calldata)
{
  SecretKeyData data = { NULL, callback, calldata };
  const gchar *gpgargv[4];
  char *uid = xstrdup (userID);
  uid = strcpy (uid, userID);
  gpgargv[0] = "--list-secret-keys";
  gpgargv[1] = "--with-colons";
  gpgargv[2] = uid;
  gpgargv[3] = NULL;
  gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
		    linecallback_id_sec, &data, callback, calldata);
  free (uid);
  if (data.key)
    {
      GpapaSecretKey *result;
      if (data.key->key)
        result = gpapa_get_secret_key_by_ID (data.key->key->KeyID,
                                             callback, calldata);
      else
        result = NULL;
      gpapa_secret_key_release (data.key);
      return result;
    }
  else
    return NULL;
}

void
gpapa_create_key_pair (GpapaPublicKey **publicKey,
		       GpapaSecretKey **secretKey, char *passphrase,
		       GpapaAlgo anAlgo, gint aKeysize, char *aUserID,
		       char *anEmail, char *aComment,
		       GpapaCallbackFunc callback, gpointer calldata)
{
  if (aKeysize && aUserID && anEmail && aComment)
    {
      const gchar *gpgargv[3];
      char *commands = NULL;
      char *commands_sprintf_str;
      char *Algo, *Sub_Algo, *Name_Comment;
      if (aComment && *aComment)
        Name_Comment = g_strdup_printf ("Name-Comment: %s\n", aComment);
      else
        Name_Comment = "";
      if (anAlgo == GPAPA_ALGO_DSA
          || anAlgo == GPAPA_ALGO_ELG_BOTH
          || anAlgo == GPAPA_ALGO_ELG)
	{
	  if (anAlgo == GPAPA_ALGO_DSA) 
	    Algo = "DSA";
	  else if (anAlgo == GPAPA_ALGO_ELG_BOTH) 
	    Algo = "ELG";
	  else /* anAlgo == GPAPA_ALGO_ELG */
	    Algo = "ELG-E";
	  commands_sprintf_str = "Key-Type: %s\n"
	                         "Key-Length: %d\n"
	                         "Name-Real: %s\n"
                                 "%s"                /* Name_Comment */
                                 "Name-Email: %s\n"
                                 "Expire-Date: 0\n"
                                 "Passphrase: %s\n"
                                 "%%commit\n";
	  commands = g_strdup_printf (commands_sprintf_str,
                                      Algo, aKeysize, 
		                      aUserID, Name_Comment, anEmail,
                                      passphrase);
	}
      else if (anAlgo == GPAPA_ALGO_BOTH)
	{
	  Algo = "DSA";
	  Sub_Algo = "ELG-E";
	  commands_sprintf_str = "Key-Type: %s\n"
                                 "Key-Length: %d\n"
                                 "Subkey-Type: %s\n"
                                 "Subkey-Length: %d\n"
                                 "Name-Real: %s\n"
                                 "%s"                /* Name_Comment */
                                 "Name-Email: %s\n"
                                 "Expire-Date: 0\n"
                                 "Passphrase: %s\n"
                                 "%%commit\n";
	  commands = g_strdup_printf (commands_sprintf_str,
                                      Algo, aKeysize, Sub_Algo, aKeysize,
                                      aUserID, Name_Comment, anEmail,
                                      passphrase);
	}
      else 
	callback (GPAPA_ACTION_ERROR, 
		  _("Specified algorithm not supported"), calldata); 
      gpgargv[0] = "--gen-key";
      gpgargv[1] = "--batch";  /* not automatically added due to commands */
      gpgargv[2] = NULL;
      gpapa_call_gnupg (gpgargv, TRUE, commands, NULL, passphrase, 
			NULL, NULL, callback, calldata);
      free (commands);
      gpapa_refresh_public_keyring (callback, calldata);
      *publicKey = gpapa_get_public_key_by_userID (aUserID, callback, calldata);
      *secretKey = gpapa_get_secret_key_by_userID (aUserID, callback, calldata);
    }
}

static void
linecallback_export_ownertrust (gchar *line, gpointer data, GpgStatusCode status)
{
  FILE *stream = data;
  if (status == NO_STATUS && stream && line)
    fprintf (stream, "%s\n", line);
}

void
gpapa_export_ownertrust (const gchar *targetFileID, GpapaArmor Armor,
			 GpapaCallbackFunc callback, gpointer calldata)
{
  if (!targetFileID)
    callback (GPAPA_ACTION_ERROR, _("Target file not specified"), calldata);
  else
    {
      FILE *stream = fopen (targetFileID, "w");
      if (!stream)
	callback (GPAPA_ACTION_ERROR,
		  "Could not open target file for writing", calldata);
      else
	{
	  const gchar *gpgargv[3];
	  int i = 0;
	  if (Armor == GPAPA_ARMOR)
	    gpgargv[i++] = "--armor";
	  gpgargv[i++] = "--export-ownertrust";
	  gpgargv[i] = NULL;
	  gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
	                    linecallback_export_ownertrust, stream, callback, calldata);
	  fclose (stream);
	}
    }
}

void
gpapa_import_ownertrust (const gchar *sourceFileID,
			 GpapaCallbackFunc callback, gpointer calldata)
{
  if (!sourceFileID)
    callback (GPAPA_ACTION_ERROR, _("Source file not specified"), calldata);
  else
    {
      const gchar *gpgargv[3];
      gchar *quoted_filename = g_strconcat ("\"", sourceFileID, "\"", NULL);
      gpgargv[0] = "--import-ownertrust";
      gpgargv[1] = (char *) quoted_filename;
      gpgargv[2] = NULL;
      gpapa_call_gnupg 	(gpgargv, TRUE, NULL, NULL, NULL,
                         NULL, NULL, callback, calldata);
      free (quoted_filename);
    }
}

void
gpapa_update_trust_database (GpapaCallbackFunc callback, gpointer calldata)
{
  const gchar *gpgargv[2];
  gpgargv[0] = "--update-trustdb";
  gpgargv[1] = NULL;
  gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
                    NULL, NULL, callback, calldata);
  gpapa_refresh_public_keyring (callback, calldata);
}

void
gpapa_import_keys (const gchar *sourceFileID,
		   GpapaCallbackFunc callback, gpointer calldata)
{
  if (!sourceFileID)
    callback (GPAPA_ACTION_ERROR, _("Source file not specified"), calldata);
  else
    {
      const gchar *gpgargv[4];
      gchar *quoted_filename = g_strconcat ("\"", sourceFileID, "\"", NULL);
      gpgargv[0] = "--allow-secret-key-import";
      gpgargv[1] = "--import";
      gpgargv[2] = (char *) quoted_filename;
      gpgargv[3] = NULL;
      gpapa_call_gnupg (gpgargv, TRUE, NULL, NULL, NULL,
	                NULL, NULL, callback, calldata);
      gpapa_refresh_public_keyring (callback, calldata);
      free (quoted_filename);
    }
}

#ifdef _WIN32

static gchar*
get_w32_clip_text (gint *r_size)
{
  gint rc, size;
  gchar *private_clip = NULL;
  gchar *clip = NULL;
  HANDLE cb;

  rc = OpenClipboard(NULL);
  if (!rc)
    return NULL;
  cb = GetClipboardData(CF_TEXT);
  if (!cb)
    goto leave;

  private_clip = GlobalLock(cb);
  if (!private_clip)
    goto leave;
  size = strlen(private_clip);

  clip = xmalloc(size + 1);
  if (!clip)
    {
      GlobalUnlock(cb);
      goto leave;
    }	
  memcpy(clip, private_clip, size);
  clip[size] = '\0';
  *r_size = size;
  GlobalUnlock(cb);

leave:
  CloseClipboard();
  return clip;
} /* get_w32_clip_text */

#endif

void
gpapa_import_keys_from_clipboard (GpapaCallbackFunc callback, gpointer calldata)
{
#ifdef _WIN32
  const gchar *gpgargv[3];
  gchar *clipboard_data;
  gint clipboard_size;
  clipboard_data = get_w32_clip_text (&clipboard_size);
  if (clipboard_data)
    {
      gpgargv[0] = "--allow-secret-key-import";
      gpgargv[1] = "--import";
      gpgargv[2] = NULL;
      gpapa_call_gnupg (gpgargv, TRUE, NULL, clipboard_data, NULL,
                        NULL, NULL, callback, calldata);
      gpapa_refresh_public_keyring (callback, calldata);
    }
#else
  fprintf (stderr, "*** Import keys from Clipboard ***\n");
  fflush (stderr);
#endif
}


/* Miscellaneous.
 */

const char *
gpapa_private_get_gpg_program (void)
{
  return gpg_program;
}

void
gpapa_init (const char *gpg)
{
  free (gpg_program);
  gpg_program = xstrdup (gpg ? gpg : "/usr/bin/gpg");
}

void
gpapa_fini (void)
{
  if (PubRing != NULL)
    {
      release_public_keyring (PubRing);
      PubRing = NULL;
    }
  if (SecRing != NULL)
    {
      release_secret_keyring (SecRing);
      SecRing = NULL;
    }
  free (gpg_program);
  gpg_program = NULL;
}

void
gpapa_idle (void)
{
  /* In the future, we will call a non-blocking select() and
   * waitpid() here to read data from an open pipe and see
   * whether gpg is still running.
   *
   * Right now, just do nothing.
   */
}
