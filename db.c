/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <string.h>
#include <unistd.h>
#include "db.h"
#include "debug.h"

GQuark
yum_db_error_quark (void)
{
    static GQuark quark;

    if (!quark)
        quark = g_quark_from_static_string ("yum_db_error");

    return quark;
}

#define ENCODED_PACKAGE_FILE_FILES 2048
#define ENCODED_PACKAGE_FILE_TYPES 60

typedef struct {
    GString *files;
    GString *types;
} EncodedPackageFile;

static EncodedPackageFile *
encoded_package_file_new (void)
{
    EncodedPackageFile *enc;

    enc = g_new0 (EncodedPackageFile, 1);
    enc->files = g_string_sized_new (ENCODED_PACKAGE_FILE_FILES);
    enc->types = g_string_sized_new (ENCODED_PACKAGE_FILE_TYPES);

    return enc;
}

static void
encoded_package_file_free (EncodedPackageFile *file)
{
    g_string_free (file->files, TRUE);
    g_string_free (file->types, TRUE);
    g_free (file);
}

static GHashTable *
package_files_to_hash (GSList *files)
{
    GHashTable *hash;
    GSList *iter;
    PackageFile *file;
    EncodedPackageFile *enc;
    char *dir;
    char *name;

    hash = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  (GDestroyNotify) g_free,
                                  (GDestroyNotify) encoded_package_file_free);

    for (iter = files; iter; iter = iter->next) {
        file = (PackageFile *) iter->data;

        dir = g_path_get_dirname (file->name);
        name = g_path_get_basename (file->name);

        enc = (EncodedPackageFile *) g_hash_table_lookup (hash, dir);
        if (!enc) {
            enc = encoded_package_file_new ();
            g_hash_table_insert (hash, dir, enc);
        } else
            g_free (dir);

        if (enc->files->len)
            g_string_append_c (enc->files, '/');
        g_string_append (enc->files, name);
        g_free (name);

        if (!strcmp (file->type, "dir"))
            g_string_append_c (enc->types, 'd');
        else if (!strcmp (file->type, "file"))
            g_string_append_c (enc->types, 'f');
        else if (!strcmp (file->type, "ghost"))
            g_string_append_c (enc->types, 'g');
    }

    return hash;
}

char *
yum_db_filename (const char *prefix)
{
    char *filename;

    filename = g_strconcat (prefix, ".sqlite", NULL);
    return filename;
}

typedef enum {
    DB_STATUS_OK,
    DB_STATUS_VERSION_MISMATCH,
    DB_STATUS_CHECKSUM_MISMATCH,
    DB_STATUS_ERROR
} DBStatus;

static DBStatus
dbinfo_status (sqlite3 *db, const char *checksum)
{
    const char *query;
    int rc;
    sqlite3_stmt *handle = NULL;
    DBStatus status = DB_STATUS_ERROR;

    query = "SELECT dbversion, checksum FROM db_info";
    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK)
        goto cleanup;

    while ((rc = sqlite3_step (handle)) == SQLITE_ROW) {
        int dbversion;
        const char *dbchecksum;

        dbversion  = sqlite3_column_int  (handle, 0);
        dbchecksum = (const char *) sqlite3_column_text (handle, 1);

        if (dbversion != YUM_SQLITE_CACHE_DBVERSION) {
            debug (DEBUG_LEVEL_INFO,
                   "Warning: cache file is version %d, we need %d, will regenerate",
                   dbversion, YUM_SQLITE_CACHE_DBVERSION);
            status = DB_STATUS_VERSION_MISMATCH;
        } else if (strcmp (checksum, dbchecksum)) {
            debug (DEBUG_LEVEL_INFO,
                   "sqlite cache needs updating, reading in metadata\n");
            status = DB_STATUS_CHECKSUM_MISMATCH;
        } else
            status = DB_STATUS_OK;

        break;
    }

 cleanup:
    if (handle)
        sqlite3_finalize (handle);

    return status;
}

static void
yum_db_create_dbinfo_table (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql = "CREATE TABLE db_info (dbversion TEXT, checksum TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create db_info table: %s",
                     sqlite3_errmsg (db));
    }
}

sqlite3 *
yum_db_open (const char *path,
             const char *checksum,
             CreateTablesFn create_tables,
             GError **err)
{
    int rc;
    sqlite3 *db = NULL;
    gboolean db_existed;

    db_existed = g_file_test (path, G_FILE_TEST_EXISTS);

    rc = sqlite3_open (path, &db);
    if (rc == SQLITE_OK) {
        if (db_existed) {
            DBStatus status = dbinfo_status (db, checksum);

            switch (status) {
            case DB_STATUS_OK:
                /* Everything is up-to-date */
                sqlite3_close (db);
                return NULL;
                break;
            case DB_STATUS_CHECKSUM_MISMATCH:
                sqlite3_exec (db, "PRAGMA synchronous = 0", NULL, NULL, NULL);
                sqlite3_exec (db, "DELETE FROM db_info", NULL, NULL, NULL);
                return db;
                break;
            case DB_STATUS_VERSION_MISMATCH:
            case DB_STATUS_ERROR:
                sqlite3_close (db);
                db = NULL;
                break;
            }
        }
    } else {
        /* Let's try to delete it and try again,
           maybe it's a sqlite3 version mismatch. */
        sqlite3_close (db);
        db = NULL;
        unlink (path);
    }

    if (!db) {
        rc = sqlite3_open (path, &db);
        if (rc != SQLITE_OK) {
            g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                         "Can not open SQL database: %s",
                         sqlite3_errmsg (db));
            goto cleanup;
        }
    }

    yum_db_create_dbinfo_table (db, err);
    if (*err)
        goto cleanup;

    create_tables (db, err);
    if (*err)
        goto cleanup;

    sqlite3_exec (db, "PRAGMA synchronous = 0", NULL, NULL, NULL);

 cleanup:
    if (*err && db) {
        sqlite3_close (db);
        db = NULL;
    }

    return db;
}

void
yum_db_dbinfo_update (sqlite3 *db, const char *checksum, GError **err)
{
    int rc;
    char *sql;

    sql = g_strdup_printf
        ("INSERT INTO db_info (dbversion, checksum) VALUES (%d, '%s')",
         YUM_SQLITE_CACHE_DBVERSION, checksum);

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not update dbinfo table: %s",
                     sqlite3_errmsg (db));

    g_free (sql);
}

GHashTable *
yum_db_read_package_ids (sqlite3 *db, GError **err)
{
    const char *query;
    int rc;
    GHashTable *hash = NULL;
    sqlite3_stmt *handle = NULL;

    query = "SELECT pkgId, pkgKey FROM packages";
    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare SQL clause: %s",
                     sqlite3_errmsg (db));
        goto cleanup;
    }

    hash = g_hash_table_new_full (g_str_hash, g_str_equal,
                                  (GDestroyNotify) g_free, NULL);

    while ((rc = sqlite3_step (handle)) == SQLITE_ROW) {
        char *pkgId;
        gint pkgKey;

        pkgId  = g_strdup ((char *) sqlite3_column_text  (handle, 0));
        pkgKey = sqlite3_column_int (handle, 1);

        g_hash_table_insert (hash, pkgId, GINT_TO_POINTER (pkgKey));
    }

    if (rc != SQLITE_DONE)
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Error reading from SQL: %s",
                     sqlite3_errmsg (db));

 cleanup:
    if (handle)
        sqlite3_finalize (handle);

    return hash;
}

void
yum_db_create_primary_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql =
        "CREATE TABLE packages ("
        "  pkgKey INTEGER PRIMARY KEY,"
        "  pkgId TEXT,"
        "  name TEXT,"
        "  arch TEXT,"
        "  version TEXT,"
        "  epoch TEXT,"
        "  release TEXT,"
        "  summary TEXT,"
        "  description TEXT,"
        "  url TEXT,"
        "  time_file TEXT,"
        "  time_build TEXT,"
        "  rpm_license TEXT,"
        "  rpm_vendor TEXT,"
        "  rpm_group TEXT,"
        "  rpm_buildhost TEXT,"
        "  rpm_sourcerpm TEXT,"
        "  rpm_header_start TEXT,"
        "  rpm_header_end TEXT,"
        "  rpm_packager TEXT,"
        "  size_package TEXT,"
        "  size_installed TEXT,"
        "  size_archive TEXT,"
        "  location_href TEXT,"
        "  location_base TEXT,"
        "  checksum_type TEXT,"
        "  checksum_value TEXT)";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create packages table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX packagename ON packages (name)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create packagename index: %s",
                     sqlite3_errmsg (db));
        return;
    }
    
    sql = "CREATE INDEX packageId ON packages (pkgId)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create packageId index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE files ("
        "  name TEXT,"
        "  type TEXT,"
        "  pkgKey TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create files table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE %s ("
        "  name TEXT,"
        "  flags TEXT,"
        "  epoch TEXT,"
        "  version TEXT,"
        "  release TEXT,"
        "  pkgKey TEXT)";

    const char *deps[] = { "requires", "provides", "conflicts", "obsoletes", NULL };
    int i;

    for (i = 0; deps[i]; i++) {
        char *query = g_strdup_printf (sql, deps[i]);
        rc = sqlite3_exec (db, query, NULL, NULL, NULL);
        g_free (query);

        if (rc != SQLITE_OK) {
            g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                         "Can not create %s table: %s",
                         deps[i], sqlite3_errmsg (db));
            return;
        }
    }

    sql = "CREATE INDEX providesname ON provides (name)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create providesname index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TRIGGER removals AFTER DELETE ON packages"
        "  BEGIN"
        "    DELETE FROM files WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM requires WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM provides WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM conflicts WHERE pkgKey = old.pkgKey;"
        "    DELETE FROM obsoletes WHERE pkgKey = old.pkgKey;"
        "  END;";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create removals trigger: %s",
                     sqlite3_errmsg (db));
        return;
    }
}

sqlite3_stmt *
yum_db_package_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query =
        "INSERT INTO packages ("
        "  pkgId, name, arch, version, epoch, release, summary, description,"
        "  url, time_file, time_build, rpm_license, rpm_vendor, rpm_group,"
        "  rpm_buildhost, rpm_sourcerpm, rpm_header_start, rpm_header_end,"
        "  rpm_packager, size_package, size_installed, size_archive,"
        "  location_href, location_base, checksum_type, checksum_value) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
        "  ?, ?, ?, ?, ?, ?, ?, ?)";

    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare packages insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}

void
yum_db_package_write (sqlite3 *db, sqlite3_stmt *handle, Package *p)
{
    int rc;

    sqlite3_bind_text (handle, 1,  p->pkgId, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 2,  p->name, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 3,  p->arch, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 4,  p->version, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 5,  p->epoch, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 6,  p->release, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 7,  p->summary, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 8,  p->description, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 9,  p->url, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 10, p->time_file, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 11, p->time_build, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 12, p->rpm_license, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 13, p->rpm_vendor, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 14, p->rpm_group, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 15, p->rpm_buildhost, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 16, p->rpm_sourcerpm, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 17, p->rpm_header_start, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 18, p->rpm_header_end, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 19, p->rpm_packager, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 20, p->size_package, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 21, p->size_installed, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 22, p->size_archive, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 23, p->location_href, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 24, p->location_base, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 25, p->checksum_type, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 26, p->checksum_value, -1, SQLITE_STATIC);

    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE) {
        debug (DEBUG_LEVEL_ERROR,
               "Error adding package to SQL: %s",
               sqlite3_errmsg (db));
    } else
        p->pkgKey = sqlite3_last_insert_rowid (db);
}

sqlite3_stmt *
yum_db_dependency_prepare (sqlite3 *db,
                           const char *table,
                           GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    char *query;

    query = g_strdup_printf
        ("INSERT INTO %s (name, flags, epoch, version, release, pkgKey) "
         "VALUES (?, ?, ?, ?, ?, ?)", table);

    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    g_free (query);

    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare dependency insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}

void
yum_db_dependency_write (sqlite3 *db,
                         sqlite3_stmt *handle,
                         gint64 pkgKey,
                         Dependency *dep)
{
    int rc;

    sqlite3_bind_text (handle, 1, dep->name,    -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 2, dep->flags,   -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 3, dep->epoch,   -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 4, dep->version, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 5, dep->release, -1, SQLITE_STATIC);
    sqlite3_bind_int  (handle, 6, pkgKey);

    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE)
        debug (DEBUG_LEVEL_ERROR,
               "Error adding dependency to SQL: %s",
               sqlite3_errmsg (db));
}

sqlite3_stmt *
yum_db_file_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query = "INSERT INTO files (name, type, pkgKey) VALUES (?, ?, ?)";

    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare file insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;

}

void
yum_db_file_write (sqlite3 *db,
                   sqlite3_stmt *handle,
                   gint64 pkgKey,
                   PackageFile *file)
{
    int rc;

    sqlite3_bind_text (handle, 1, file->type, -1, SQLITE_STATIC);
    sqlite3_bind_text (handle, 2, file->name, -1, SQLITE_STATIC);
    sqlite3_bind_int  (handle, 3, pkgKey);

    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE)
        debug (DEBUG_LEVEL_ERROR,
               "Error adding package file to SQL: %s",
               sqlite3_errmsg (db));
}

void
yum_db_create_filelist_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql =
        "CREATE TABLE packages ("
        "  pkgKey INTEGER PRIMARY KEY,"
        "  pkgId TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create packages table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE filelist ("
        "  pkgKey INTEGER,"
        "  dirname TEXT,"
        "  filenames TEXT,"
        "  filetypes TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create filelist table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX keyfile ON filelist (pkgKey)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create keyfile index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX pkgId ON packages (pkgId)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create pkgId index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TRIGGER remove_filelist AFTER DELETE ON packages"
        "  BEGIN"
        "    DELETE FROM filelist WHERE pkgKey = old.pkgKey;"
        "  END;";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create remove_filelist trigger: %s",
                     sqlite3_errmsg (db));
        return;
    }
}

sqlite3_stmt *
yum_db_package_ids_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query = "INSERT INTO packages (pkgId) VALUES (?)";
    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare package ids insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}

void
yum_db_package_ids_write (sqlite3 *db, sqlite3_stmt *handle, Package *p)
{
    int rc;

    sqlite3_bind_text (handle, 1,  p->pkgId, -1, SQLITE_STATIC);
    rc = sqlite3_step (handle);
    sqlite3_reset (handle);

    if (rc != SQLITE_DONE) {
        debug (DEBUG_LEVEL_ERROR,
               "Error adding package to SQL: %s",
               sqlite3_errmsg (db));
    } else
        p->pkgKey = sqlite3_last_insert_rowid (db);
}

sqlite3_stmt *
yum_db_filelists_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query =
        "INSERT INTO filelist (pkgKey, dirname, filenames, filetypes) "
        " VALUES (?, ?, ?, ?)";

    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare filelist insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *handle;
    gint64 pkgKey;
} FileWriteInfo;

static void
write_file (gpointer key, gpointer value, gpointer user_data)
{
    EncodedPackageFile *file = (EncodedPackageFile *) value;
    FileWriteInfo *info = (FileWriteInfo *) user_data;
    int rc;

    sqlite3_bind_int  (info->handle, 1, info->pkgKey);
    sqlite3_bind_text (info->handle, 2, (const char *) key, -1, SQLITE_STATIC);
    sqlite3_bind_text (info->handle, 3, file->files->str, -1, SQLITE_STATIC);
    sqlite3_bind_text (info->handle, 4, file->types->str, -1, SQLITE_STATIC);

    rc = sqlite3_step (info->handle);
    sqlite3_reset (info->handle);

    if (rc != SQLITE_DONE) {
        debug (DEBUG_LEVEL_ERROR,
               "Error adding file to SQL: %s",
               sqlite3_errmsg (info->db));
    }
}

void
yum_db_filelists_write (sqlite3 *db, sqlite3_stmt *handle, Package *p)
{
    GHashTable *hash;
    FileWriteInfo info;

    info.db = db;
    info.handle = handle;
    info.pkgKey = p->pkgKey;

    hash = package_files_to_hash (p->files);
    g_hash_table_foreach (hash, write_file, &info);
    g_hash_table_destroy (hash);
}

void
yum_db_create_other_tables (sqlite3 *db, GError **err)
{
    int rc;
    const char *sql;

    sql =
        "CREATE TABLE packages ("
        "  pkgKey INTEGER PRIMARY KEY,"
        "  pkgId TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create packages table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TABLE changelog ("
        "  pkgKey INTEGER,"
        "  author TEXT,"
        "  date TEXT,"
        "  changelog TEXT)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create changelog table: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX keychange ON changelog (pkgKey)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create keychange index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql = "CREATE INDEX pkgId ON packages (pkgId)";
    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create pkgId index: %s",
                     sqlite3_errmsg (db));
        return;
    }

    sql =
        "CREATE TRIGGER remove_changelogs AFTER DELETE ON packages"
        "  BEGIN"
        "    DELETE FROM changelog WHERE pkgKey = old.pkgKey;"
        "  END;";

    rc = sqlite3_exec (db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not create remove_changelogs trigger: %s",
                     sqlite3_errmsg (db));
        return;
    }
}

sqlite3_stmt *
yum_db_changelog_prepare (sqlite3 *db, GError **err)
{
    int rc;
    sqlite3_stmt *handle = NULL;
    const char *query;

    query =
        "INSERT INTO changelog (pkgKey, author, date, changelog) "
        " VALUES (?, ?, ?, ?)";

    rc = sqlite3_prepare (db, query, -1, &handle, NULL);
    if (rc != SQLITE_OK) {
        g_set_error (err, YUM_DB_ERROR, YUM_DB_ERROR,
                     "Can not prepare changelog insertion: %s",
                     sqlite3_errmsg (db));
        sqlite3_finalize (handle);
        handle = NULL;
    }

    return handle;
}

void
yum_db_changelog_write (sqlite3 *db, sqlite3_stmt *handle, Package *p)
{
    GSList *iter;
    ChangelogEntry *entry;
    int rc;

    for (iter = p->changelogs; iter; iter = iter->next) {
        entry = (ChangelogEntry *) iter->data;

        sqlite3_bind_int  (handle, 1, p->pkgKey);
        sqlite3_bind_text (handle, 2, entry->author, -1, SQLITE_STATIC);
        sqlite3_bind_text (handle, 3, entry->date, -1, SQLITE_STATIC);
        sqlite3_bind_text (handle, 4, entry->changelog, -1, SQLITE_STATIC);

        rc = sqlite3_step (handle);
        sqlite3_reset (handle);

        if (rc != SQLITE_DONE) {
            debug (DEBUG_LEVEL_ERROR,
                   "Error adding changelog to SQL: %s",
                   sqlite3_errmsg (db));
        }
    }
}
