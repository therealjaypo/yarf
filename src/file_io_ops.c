/*
 * Copyright (C) 2012-2014 Paul Ionkin <paul.ionkin@gmail.com>
 * Copyright (C) 2012-2014 Skoobe GmbH. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "file_io_ops.h"
#include "http_connection.h"
#include "cache_mng.h"
#include "utils.h"
#include "dir_tree.h"

/*{{{ struct */
struct _FileIO {
    Application *app;
    gchar *fname;
    gchar *content_type;
    fuse_ino_t ino;
    gboolean assume_new; // assume file does not exist yet

    // write
    guint64 current_size;
    struct evbuffer *write_buf;
    gboolean multipart_initiated;
    gchar *uploadid;
    guint part_number;
    GList *l_parts; // list of FileIOPart
    MD5_CTX md5;

    // read
    gboolean head_req_sent;
    guint64 file_size;
};

typedef struct {
    guint part_number;
    gchar *md5str;
    gchar *md5b;
} FileIOPart;
/*}}}*/

#define FIO_LOG "fio"

/*{{{ create / destroy */

FileIO *fileio_create (Application *app, const gchar *fname, fuse_ino_t ino, gboolean assume_new)
{
    FileIO *fop;

    fop = g_new0 (FileIO, 1);
    fop->app = app;
    fop->current_size = 0;
    fop->write_buf = evbuffer_new ();
    fop->fname = g_strdup_printf ("/%s", fname);
    fop->content_type = NULL;
    fop->file_size = 0;
    fop->head_req_sent = FALSE;
    fop->multipart_initiated = FALSE;
    fop->uploadid = NULL;
    fop->l_parts = NULL;
    fop->ino = ino;
    fop->assume_new = assume_new;
    MD5_Init (&fop->md5);

    return fop;
}

void fileio_destroy (FileIO *fop)
{
    GList *l;

    for (l = g_list_first (fop->l_parts); l; l = g_list_next (l)) {
        FileIOPart *part = (FileIOPart *) l->data;
        g_free (part->md5str);
        g_free (part->md5b);
        g_free (part);
    }
    evbuffer_free (fop->write_buf);
    g_free (fop->fname);
    if (fop->content_type)
        g_free (fop->content_type);
    if (fop->uploadid)
        g_free (fop->uploadid);
    g_free (fop);
}
/*}}}*/

/*{{{ fileio_release*/

/*{{{ update headers on uploaded object */
static void fileio_release_on_update_header_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileIO *fop = (FileIO *) ctx;

    http_connection_release (con);

    if (!success) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to update headers on the server !", INO_T (fop->ino), con);
        fileio_destroy (fop);
        return;
    }

    // done
    LOG_debug (FIO_LOG, INO_CON_H"Headers are updated !", INO_T (fop->ino), con);

    fileio_destroy (fop);
}

// got HttpConnection object
static void fileio_release_on_update_headers_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileIO *fop = (FileIO *) ctx;
    gchar *path;
    gchar *cpy_path;
    gboolean res;
    unsigned char digest[16];
    gchar *md5str;
    size_t i;

    LOG_debug (FIO_LOG, INO_CON_H"Updating object's headers..", INO_T (fop->ino), con);

    http_connection_acquire (con);

    if (fop->content_type)
        http_connection_add_output_header (con, "Content-Type", fop->content_type);
    http_connection_add_output_header (con, "x-amz-metadata-directive", "REPLACE");
    http_connection_add_output_header (con, "x-amz-storage-class", conf_get_string (application_get_conf (con->app), "s3.storage_type"));

    MD5_Final (digest, &fop->md5);
    md5str = g_malloc (33);
    for (i = 0; i < 16; ++i)
        sprintf(&md5str[i*2], "%02x", (unsigned int)digest[i]);
    http_connection_add_output_header (con, "x-amz-meta-md5", md5str);
    g_free (md5str);

    cpy_path = g_strdup_printf ("%s%s", conf_get_string (application_get_conf (fop->app), "s3.bucket_name"), fop->fname);
    http_connection_add_output_header (con, "x-amz-copy-source", cpy_path);
    g_free (cpy_path);

    path = g_strdup_printf ("%s", fop->fname);
    res = http_connection_make_request (con,
        path, "PUT", NULL, TRUE, NULL,
        fileio_release_on_update_header_cb,
        fop
    );
    g_free (path);

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (fop->ino), con);
        http_connection_release (con);
        fileio_destroy (fop);
        return;
    }
}

static void fileio_release_update_headers (FileIO *fop)
{
    // update MD5 headers only if versioning is disabled
    if (conf_get_boolean (application_get_conf (fop->app), "s3.versioning")) {
        LOG_debug (FIO_LOG, INO_H"File uploaded !", INO_T (fop->ino));
        fileio_destroy (fop);
    } else {
        if (!client_pool_get_client (application_get_write_client_pool (fop->app),
            fileio_release_on_update_headers_con_cb, fop)) {
            LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (fop->ino));
            fileio_destroy (fop);
            return;
        }
    }
}
/*}}}*/

/*{{{ Complete Multipart Upload */
// multipart is sent
static void fileio_release_on_complete_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileIO *fop = (FileIO *) ctx;
    const gchar *versioning_header;

    http_connection_release (con);

    if (!success) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to send Multipart data to the server !", INO_T (fop->ino), con);
        fileio_destroy (fop);
        return;
    }

    versioning_header = http_find_header (headers, "x-amz-version-id");
    if (versioning_header) {
        cache_mng_update_version_id (application_get_cache_mng (fop->app),
            fop->ino, versioning_header);
    }

    // done
    LOG_debug (FIO_LOG, INO_CON_H"Multipart Upload is done !", INO_T (fop->ino), con);

    // fileio_destroy (fop);
    fileio_release_update_headers (fop);
}

// got HttpConnection object
static void fileio_release_on_complete_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileIO *fop = (FileIO *) ctx;
    gchar *path;
    gboolean res;
    struct evbuffer *xml_buf;
    GList *l;

    xml_buf = evbuffer_new ();
    evbuffer_add_printf (xml_buf, "%s", "<CompleteMultipartUpload>");
    for (l = g_list_first (fop->l_parts); l; l = g_list_next (l)) {
        FileIOPart *part = (FileIOPart *) l->data;
        evbuffer_add_printf (xml_buf,
            "<Part><PartNumber>%u</PartNumber><ETag>\"%s\"</ETag></Part>",
            part->part_number, part->md5str);
    }
    evbuffer_add_printf (xml_buf, "%s", "</CompleteMultipartUpload>");

    LOG_debug (FIO_LOG, INO_CON_H"Sending Multipart Final part..", INO_T (fop->ino), con);

    http_connection_acquire (con);

    path = g_strdup_printf ("%s?uploadId=%s",
        fop->fname, fop->uploadid);
    res = http_connection_make_request (con,
        path, "POST", xml_buf, TRUE, NULL,
        fileio_release_on_complete_cb,
        fop
    );
    g_free (path);
    evbuffer_free (xml_buf);

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (fop->ino), con);
        http_connection_release (con);
        fileio_destroy (fop);
        return;
    }
}

static void fileio_release_complete_multipart (FileIO *fop)
{
    if (!fop->uploadid) {
        LOG_err (FIO_LOG, INO_H"UploadID is not set, aborting operation !", INO_T (fop->ino));
        fileio_destroy (fop);
        return;
    }

    if (!client_pool_get_client (application_get_write_client_pool (fop->app),
        fileio_release_on_complete_con_cb, fop)) {
        LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (fop->ino));
        fileio_destroy (fop);
        return;
     }
}
/*}}}*/

/*{{{ sent part*/
// file is sent
static void fileio_release_on_part_sent_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileIO *fop = (FileIO *) ctx;
    const gchar *versioning_header;

    http_connection_release (con);

    if (!success) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to send bufer to server !", INO_T (fop->ino), con);
        fileio_destroy (fop);
        return;
    }

    versioning_header = http_find_header (headers, "x-amz-version-id");
    if (versioning_header) {
        cache_mng_update_version_id (application_get_cache_mng (fop->app),
            fop->ino, versioning_header);
    }
    // if it's a multi part upload - Complete Multipart Upload
    if (fop->multipart_initiated) {
        fileio_release_complete_multipart (fop);

    // or we are done
    } else {
        fileio_release_update_headers (fop);
        //fileio_destroy (fop);
    }
}

// got HttpConnection object
static void fileio_release_on_part_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileIO *fop = (FileIO *) ctx;
    gchar *path;
    gboolean res;
    FileIOPart *part;
    size_t buf_len;
    const gchar *buf;

    LOG_debug (FIO_LOG, INO_CON_H"Releasing fop. Size: %zu", INO_T (fop->ino), con, evbuffer_get_length (fop->write_buf));

    // add part information to the list
    part = g_new0 (FileIOPart, 1);
    part->part_number = fop->part_number;
    buf_len = evbuffer_get_length (fop->write_buf);
    buf = (const gchar *)evbuffer_pullup (fop->write_buf, buf_len);

    // XXX: move to separate thread
    // 1. calculate MD5 of a part.
    get_md5_sum (buf, buf_len, &part->md5str, &part->md5b);
    // 2. calculate MD5 of multiple message blocks
    MD5_Update (&fop->md5, buf, buf_len);

    fop->l_parts = g_list_append (fop->l_parts, part);

    // if this is a multipart
    if (fop->multipart_initiated) {

        if (!fop->uploadid) {
            LOG_err (FIO_LOG, INO_CON_H"UploadID is not set, aborting operation !", INO_T (fop->ino), con);
            fileio_destroy (fop);
            return;
        }

        path = g_strdup_printf ("%s?partNumber=%u&uploadId=%s",
            fop->fname, fop->part_number, fop->uploadid);
        fop->part_number++;

    } else {
        path = g_strdup (fop->fname);
    }

#ifdef MAGIC_ENABLED
    // guess MIME type
    gchar *mime_type = magic_buffer (application_get_magic_ctx (fop->app), buf, buf_len);
    if (mime_type) {
        LOG_debug (FIO_LOG, "Guessed MIME type of %s as %s", path, mime_type);
        fop->content_type = g_strdup (mime_type);
    } else {
        LOG_err (FIO_LOG, "Failed to guess MIME type of %s !", path);
    }
#endif

#ifdef USE_MIMETYPES
    gchar *mime_type = (gchar *)mimetypes_find(path);
    if( mime_type ) {
        LOG_debug(FIO_LOG,"mime.types says %s is %s",path,mime_type);
	fop->content_type = strdup(mime_type);
    }
#endif

    http_connection_acquire (con);

    // add output headers
    http_connection_add_output_header (con, "Content-MD5", part->md5b);
    if (fop->content_type)
        http_connection_add_output_header (con, "Content-Type", fop->content_type);


    // if this is the full file
    if (!fop->multipart_initiated) {
        time_t t;
        gchar time_str[50];

        // Add current time
        t = time (NULL);
        if (strftime (time_str, sizeof (time_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t))) {
            http_connection_add_output_header (con, "x-amz-meta-date", time_str);
        }

        http_connection_add_output_header (con, "x-amz-storage-class", conf_get_string (application_get_conf (con->app), "s3.storage_type"));
    }

    res = http_connection_make_request (con,
        path, "PUT", fop->write_buf, TRUE, NULL,
        fileio_release_on_part_sent_cb,
        fop
    );
    g_free (path);

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (fop->ino), con);
        http_connection_release (con);
        fileio_destroy (fop);
        return;
    }
}
/*}}}*/

// file is released, finish all operations
void fileio_release (FileIO *fop)
{
    // if write buffer has some data left - send it to the server
    // or an empty file was created
    if (evbuffer_get_length (fop->write_buf) || fop->assume_new) {
        if (!client_pool_get_client (application_get_write_client_pool (fop->app),
            fileio_release_on_part_con_cb, fop)) {
            LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (fop->ino));
            fileio_destroy (fop);
            return;
        }
    } else {
        // if it's a multi part upload - Complete Multipart Upload
        if (fop->multipart_initiated) {
            fileio_release_complete_multipart (fop);

        // just a "small" file
        } else
            fileio_destroy (fop);
    }
}
/*}}}*/

/*{{{ fileio_write_buffer */

typedef struct {
    FileIO *fop;
    size_t buf_size;
    off_t off;
    fuse_ino_t ino;
    FileIO_on_buffer_written_cb on_buffer_written_cb;
    gpointer ctx;
} FileWriteData;

/*{{{ send part */

// buffer is sent
static void fileio_write_on_send_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileWriteData *wdata = (FileWriteData *) ctx;
    const char *versioning_header;

    http_connection_release (con);

    if (!success) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to send bufer to server !", INO_T (wdata->ino), con);
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }

    versioning_header = http_find_header (headers, "x-amz-version-id");
    if (versioning_header) {
        cache_mng_update_version_id (application_get_cache_mng (wdata->fop->app),
            wdata->ino, versioning_header);
    }

    // empty part buffer
    evbuffer_drain (wdata->fop->write_buf, -1);

    // done sending part
    wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, TRUE, wdata->buf_size);
    g_free (wdata);
}

// got HttpConnection object
static void fileio_write_on_send_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileWriteData *wdata = (FileWriteData *) ctx;
    gchar *path;
    gboolean res;
    FileIOPart *part;
    size_t buf_len;
    const gchar *buf;

    http_connection_acquire (con);

    // add part information to the list
    part = g_new0 (FileIOPart, 1);
    part->part_number = wdata->fop->part_number;
    buf_len = evbuffer_get_length (wdata->fop->write_buf);
    buf = (const gchar *) evbuffer_pullup (wdata->fop->write_buf, buf_len);

    // XXX: move to separate thread
    // 1. calculate MD5 of a part.
    get_md5_sum (buf, buf_len, &part->md5str, &part->md5b);
    // 2. calculate MD5 of multiple message blocks
    MD5_Update (&wdata->fop->md5, buf, buf_len);

    wdata->fop->l_parts = g_list_append (wdata->fop->l_parts, part);

    path = g_strdup_printf ("%s?partNumber=%u&uploadId=%s",
        wdata->fop->fname, wdata->fop->part_number, wdata->fop->uploadid);

    // increase part number
    wdata->fop->part_number++;
    // XXX: check that part_number does not exceeds 10000

    // add output headers
    http_connection_add_output_header (con, "Content-MD5", part->md5b);

    res = http_connection_make_request (con,
        path, "PUT", wdata->fop->write_buf, TRUE, NULL,
        fileio_write_on_send_cb,
        wdata
    );
    g_free (path);

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (wdata->ino), con);
        http_connection_release (con);
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }
}

static void fileio_write_send_part (FileWriteData *wdata)
{
    if (!wdata->fop->uploadid) {
        LOG_err (FIO_LOG, INO_H"UploadID is not set, aborting operation !", INO_T (wdata->ino));
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }

    if (!client_pool_get_client (application_get_write_client_pool (wdata->fop->app),
        fileio_write_on_send_con_cb, wdata)) {
        LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (wdata->ino));
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }
}
/*}}}*/

/*{{{ Multipart Init */

static gchar *get_uploadid (const char *xml, size_t xml_len) {
    xmlDocPtr doc;
    xmlXPathContextPtr ctx;
    xmlXPathObjectPtr uploadid_xp;
    xmlNodeSetPtr nodes;
    gchar *uploadid = NULL;

    doc = xmlReadMemory (xml, xml_len, "", NULL, 0);
    ctx = xmlXPathNewContext (doc);
    xmlXPathRegisterNs (ctx, (xmlChar *) "s3", (xmlChar *) "http://s3.amazonaws.com/doc/2006-03-01/");

    uploadid_xp = xmlXPathEvalExpression ((xmlChar *) "//s3:UploadId", ctx);
    if (!uploadid_xp) {
        LOG_err (FIO_LOG, "S3 returned incorrect XML !");
        xmlXPathFreeContext (ctx);
        xmlFreeDoc (doc);
        return NULL;
    }

    nodes = uploadid_xp->nodesetval;
    if (!nodes) {
        LOG_err (FIO_LOG, "S3 returned incorrect XML !");
        xmlXPathFreeObject (uploadid_xp);
        xmlXPathFreeContext (ctx);
        xmlFreeDoc (doc);
        return NULL;
    }

    if (!nodes || nodes->nodeNr < 1) {
        uploadid = NULL;
    } else {
        uploadid = (char *) xmlNodeListGetString (doc, nodes->nodeTab[0]->xmlChildrenNode, 1);
    }

    xmlXPathFreeObject (uploadid_xp);
    xmlXPathFreeContext (ctx);
    xmlFreeDoc (doc);

    return uploadid;
}

static void fileio_write_on_multipart_init_cb (HttpConnection *con, void *ctx, gboolean success,
    const gchar *buf, size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileWriteData *wdata = (FileWriteData *) ctx;
    gchar *uploadid;

    http_connection_release (con);

    wdata->fop->multipart_initiated = TRUE;

    if (!success || !buf_len) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to get multipart init data from the server !", INO_T (wdata->ino), con);
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }

    uploadid = get_uploadid (buf, buf_len);
    if (!uploadid) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to parse multipart init data!", INO_T (wdata->ino), con);
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }
    wdata->fop->uploadid = g_strdup (uploadid);
    xmlFree (uploadid);

    // done, resume uploading part
    wdata->fop->part_number = 1;
    fileio_write_send_part (wdata);
}

// got HttpConnection object
static void fileio_write_on_multipart_init_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileWriteData *wdata = (FileWriteData *) ctx;
    gboolean res;
    gchar *path;

    http_connection_acquire (con);

    path = g_strdup_printf ("%s?uploads", wdata->fop->fname);

    // send storage class with the init request
    http_connection_add_output_header (con, "x-amz-storage-class", conf_get_string (application_get_conf (con->app), "s3.storage_type"));

    res = http_connection_make_request (con,
        path, "POST", NULL, TRUE, NULL,
        fileio_write_on_multipart_init_cb,
        wdata
    );
    g_free (path);

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (wdata->ino), con);
        http_connection_release (con);
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }
}

static void fileio_write_init_multipart (FileWriteData *wdata)
{
    if (!client_pool_get_client (application_get_write_client_pool (wdata->fop->app),
        fileio_write_on_multipart_init_con_cb, wdata)) {
        LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (wdata->ino));
        wdata->on_buffer_written_cb (wdata->fop, wdata->ctx, FALSE, 0);
        g_free (wdata);
        return;
    }
}
/*}}}*/

void fileio_write_buffer (FileIO *fop,
    const char *buf, size_t buf_size, off_t off, fuse_ino_t ino,
    FileIO_on_buffer_written_cb on_buffer_written_cb, gpointer ctx)
{
    FileWriteData *wdata;

    // XXX: allow only sequentially write
    // current written bytes should be always match offset
    if (off >= 0 && fop->current_size != (guint64)off) {
        LOG_err (FIO_LOG, INO_H"Write call with offset %"OFF_FMT" is not allowed !", INO_T (ino), off);
        on_buffer_written_cb (fop, ctx, FALSE, 0);
        return;
    }

    // add data to output buffer
    evbuffer_add (fop->write_buf, buf, buf_size);
    fop->current_size += buf_size;

    LOG_debug (FIO_LOG, INO_H"Write buf size: %zd", INO_T (ino), evbuffer_get_length (fop->write_buf));

    // CacheMng
    cache_mng_store_file_buf (application_get_cache_mng (fop->app),
        ino, buf_size, off, (unsigned char *) buf,
        NULL, NULL);

    // if current write buffer exceeds "part_size" - this is a multipart upload
    if (evbuffer_get_length (fop->write_buf) >= conf_get_uint (application_get_conf (fop->app), "s3.part_size")) {
        // init helper struct
        wdata = g_new0 (FileWriteData, 1);
        wdata->fop = fop;
        wdata->buf_size = buf_size;
        wdata->off = off;
        wdata->ino = ino;
        wdata->on_buffer_written_cb = on_buffer_written_cb;
        wdata->ctx = ctx;

        // init multipart upload
        if (!fop->multipart_initiated) {
            fileio_write_init_multipart (wdata);

        // else send the current part
        } else {
            fileio_write_send_part (wdata);
        }

    // or just notify client that we are ready for more data
    } else {
        on_buffer_written_cb (fop, ctx, TRUE, buf_size);
    }
}
/*}}}*/

/*{{{ fileio_read_buffer*/

typedef struct {
    FileIO *fop;
    guint64 size;
    off_t off;
    fuse_ino_t ino;
    off_t request_offset;
    FileIO_on_buffer_read_cb on_buffer_read_cb;
    gpointer ctx;
} FileReadData;

static void fileio_read_get_buf (FileReadData *rdata);

/*{{{ GET request */
static void fileio_read_on_get_cb (HttpConnection *con, void *ctx, gboolean success,
    const gchar *buf, size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileReadData *rdata = (FileReadData *) ctx;
    const char *versioning_header = NULL;

    // release HttpConnection
    http_connection_release (con);

    if (!success) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to get file from server !", INO_T (rdata->ino), con);
        rdata->on_buffer_read_cb (rdata->ctx, FALSE, NULL, 0);
        g_free (rdata);
        return;
    }

    // store it in the local cache
    cache_mng_store_file_buf (application_get_cache_mng (rdata->fop->app),
        rdata->ino, buf_len, rdata->request_offset, (unsigned char *) buf,
        NULL, NULL);

    // update version ID
    versioning_header = http_find_header (headers, "x-amz-version-id");
    if (versioning_header) {
        cache_mng_update_version_id (application_get_cache_mng (rdata->fop->app), rdata->ino, versioning_header);
    }

    LOG_debug (FIO_LOG, INO_H"Storing [%"G_GUINT64_FORMAT" %zu]", INO_T(rdata->ino), rdata->request_offset, buf_len);

    // and read it
    fileio_read_get_buf (rdata);
}

// got HttpConnection object
static void fileio_read_on_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileReadData *rdata = (FileReadData *) ctx;
    gboolean res;
    guint64 part_size;

    http_connection_acquire (con);

    part_size = conf_get_uint (application_get_conf (rdata->fop->app), "s3.part_size");

    // small file - get the whole file at once
    if (rdata->fop->file_size < part_size)
        rdata->request_offset = 0;

    // calculate offset
    else {
        gchar *range_hdr;

        if (part_size < rdata->size)
            part_size = rdata->size;

        rdata->request_offset = rdata->off;
        range_hdr = g_strdup_printf ("bytes=%"G_GUINT64_FORMAT"-%"G_GUINT64_FORMAT,
            (gint64)rdata->request_offset, (gint64)(rdata->request_offset + part_size));
        http_connection_add_output_header (con, "Range", range_hdr);
        g_free (range_hdr);
    }

    res = http_connection_make_request (con,
        rdata->fop->fname, "GET", NULL, TRUE, NULL,
        fileio_read_on_get_cb,
        rdata
    );

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (rdata->ino), con);
        http_connection_release (con);
        rdata->on_buffer_read_cb (rdata->ctx, FALSE, NULL, 0);
        g_free (rdata);
        return;
    }
}

static void fileio_read_on_cache_cb (unsigned char *buf, size_t size, gboolean success, void *ctx)
{
    FileReadData *rdata = (FileReadData *) ctx;

    // we got data from the cache
    if (success) {
        LOG_debug (FIO_LOG, INO_H"Reading from cache", INO_T (rdata->ino));
        rdata->on_buffer_read_cb (rdata->ctx, TRUE, (char *)buf, size);
        g_free (rdata);
    } else {
        LOG_debug (FIO_LOG, INO_H"Reading from server !", INO_T (rdata->ino));
        if (!client_pool_get_client (application_get_read_client_pool (rdata->fop->app), fileio_read_on_con_cb, rdata)) {
            LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (rdata->ino));
            rdata->on_buffer_read_cb (rdata->ctx, FALSE, NULL, 0);
            g_free (rdata);
            return;
        }
    }
}

static void fileio_read_get_buf (FileReadData *rdata)
{
    if ((guint64)rdata->off >= rdata->fop->file_size) {
        // requested range is outsize the file size
        LOG_debug (FIO_LOG, INO_H"requested size is beyond the file size!", INO_T (rdata->ino));
        fileio_read_on_cache_cb (NULL, 0, TRUE, rdata);
        return;
    }

    // set new request size:
    // 1. file must have some size
    // 2. offset must be less than the file size
    // 3. offset + size is greater than the file size
    if (rdata->fop->file_size > 0 &&
        rdata->off >= 0 &&
        rdata->fop->file_size > (guint64)rdata->off &&
        (guint64) (rdata->off + rdata->size) > rdata->fop->file_size)
    {
        rdata->size = rdata->fop->file_size - rdata->off;
    // special case, when zero-size file is requested
    } else if (rdata->fop->file_size == 0) {
        rdata->size = 0;
    } else {
        // request size and offset are ok
    }

    LOG_debug (FIO_LOG, INO_H"requesting [%"OFF_FMT": %"G_GUINT64_FORMAT"], file size: %"G_GUINT64_FORMAT,
        INO_T (rdata->ino), rdata->off, rdata->size, rdata->fop->file_size);

    cache_mng_retrieve_file_buf (application_get_cache_mng (rdata->fop->app),
        rdata->ino, rdata->size, rdata->off,
        fileio_read_on_cache_cb, rdata);
}
/*}}}*/

/*{{{ HEAD request*/

static void fileio_read_on_head_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    struct evkeyvalq *headers)
{
    FileReadData *rdata = (FileReadData *) ctx;
    const char *content_len_header;
    DirTree *dtree;

    // release HttpConnection
    http_connection_release (con);

    if (!success) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to get HEAD from server !", INO_T (rdata->ino), con);
        rdata->on_buffer_read_cb (rdata->ctx, FALSE, NULL, 0);
        g_free (rdata);
        return;
    }

    rdata->fop->head_req_sent = TRUE;
    // update DirTree
    dtree = application_get_dir_tree (rdata->fop->app);
    dir_tree_set_entry_exist (dtree, rdata->ino);

    // consistency checking:

    // 1. check local and remote file sizes
    content_len_header = http_find_header (headers, "Content-Length");
    if (content_len_header) {
        guint64 local_size = 0;
        gint64 size = 0;

        size = strtoll ((char *)content_len_header, NULL, 10);
        if (size < 0) {
            LOG_err (FIO_LOG, INO_CON_H"Header contains incorrect file size!", INO_T (rdata->ino), con);
            size = 0;
        }

        rdata->fop->file_size = size;
        LOG_debug (FIO_LOG, INO_H"Remote file size: %"G_GUINT64_FORMAT, INO_T (rdata->ino), rdata->fop->file_size);

        local_size = cache_mng_get_file_length (application_get_cache_mng (rdata->fop->app), rdata->ino);
        if (local_size != rdata->fop->file_size) {
            LOG_debug (FIO_LOG, INO_H"Local and remote file sizes do not match, invalidating local cached file!",
                INO_T (rdata->ino));
            cache_mng_remove_file (application_get_cache_mng (rdata->fop->app), rdata->ino);
        }
    }

    // 2. use one of the following ways to check that local and remote files are identical
    // if versioning is enabled: compare version IDs
    // if bucket has versioning disabled: compare MD5 sums
    if (conf_get_boolean (application_get_conf (rdata->fop->app), "s3.versioning")) {
        const char *versioning_header = http_find_header (headers, "x-amz-version-id");
        if (versioning_header) {
            const gchar *local_version_id = cache_mng_get_version_id (application_get_cache_mng (rdata->fop->app), rdata->ino);
            if (local_version_id && !strcmp (local_version_id, versioning_header)) {
                LOG_debug (FIO_LOG, INO_H"Both version IDs match, using local cached file!", INO_T (rdata->ino));
            } else {
                LOG_debug (FIO_LOG, INO_H"Version IDs do not match, invalidating local cached file!: %s %s",
                    INO_T (rdata->ino), local_version_id, versioning_header);
                cache_mng_remove_file (application_get_cache_mng (rdata->fop->app), rdata->ino);
            }

        // header was not found
        } else {
            LOG_debug (FIO_LOG, INO_H"Versioning header was not found, invalidating local cached file!", INO_T (rdata->ino));
            cache_mng_remove_file (application_get_cache_mng (rdata->fop->app), rdata->ino);
        }

    //check for MD5
    } else  {
        const char *md5_header = http_find_header (headers, "x-amz-meta-md5");
        if (md5_header) {
            gchar *md5str = NULL;

            // at this point we have both remote and local MD5 sums
            if (cache_mng_get_md5 (application_get_cache_mng (rdata->fop->app), rdata->ino, &md5str)) {
                if (!strncmp (md5_header, md5str, 32)) {
                    LOG_debug (FIO_LOG, INO_H"MD5 sums match, using local cached file!", INO_T (rdata->ino));
                } else {
                    LOG_debug (FIO_LOG, INO_H"MD5 sums do not match, invalidating local cached file!", INO_T (rdata->ino));
                    cache_mng_remove_file (application_get_cache_mng (rdata->fop->app), rdata->ino);
                }
            } else {
                LOG_debug (FIO_LOG, INO_H"Failed to get local MD5 sum, invalidating local cached file!", INO_T (rdata->ino));
                cache_mng_remove_file (application_get_cache_mng (rdata->fop->app), rdata->ino);
            }

            if (md5str)
                g_free (md5str);

        // header was not found
        } else {
            LOG_debug (FIO_LOG, INO_H"MD5 sum header was not found, invalidating local cached file!", INO_T (rdata->ino));
            cache_mng_remove_file (application_get_cache_mng (rdata->fop->app), rdata->ino);
        }
    }

    // resume downloading file
    fileio_read_get_buf (rdata);
}

// got HttpConnection object
static void fileio_read_on_head_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileReadData *rdata = (FileReadData *) ctx;
    gboolean res;

    http_connection_acquire (con);

    res = http_connection_make_request (con,
        rdata->fop->fname, "HEAD", NULL, FALSE, NULL,
        fileio_read_on_head_cb,
        rdata
    );

    if (!res) {
        LOG_err (FIO_LOG, INO_CON_H"Failed to create HTTP request !", INO_T (rdata->ino), con);
        http_connection_release (con);
        rdata->on_buffer_read_cb (rdata->ctx, FALSE, NULL, 0);
        g_free (rdata);
        return;
    }
}
/*}}}*/

// if it's the first fuse read() request - send HEAD request to server
// else try to get data from local cache, otherwise download from the server
void fileio_read_buffer (FileIO *fop,
    size_t size, off_t off, fuse_ino_t ino,
    FileIO_on_buffer_read_cb on_buffer_read_cb, gpointer ctx)
{
    FileReadData *rdata;

    rdata = g_new0 (FileReadData, 1);
    rdata->fop = fop;
    rdata->size = size;
    rdata->off = off;
    rdata->ino = ino;
    rdata->on_buffer_read_cb = on_buffer_read_cb;
    rdata->ctx = ctx;
    rdata->request_offset = off;

    // send HEAD request first
    if (!rdata->fop->head_req_sent) {
         // get HTTP connection to download manifest or a full file
        if (!client_pool_get_client (application_get_read_client_pool (rdata->fop->app), fileio_read_on_head_con_cb, rdata)) {
            LOG_err (FIO_LOG, INO_H"Failed to get HTTP client !", INO_T (rdata->ino));
            rdata->on_buffer_read_cb (rdata->ctx, FALSE, NULL, 0);
            g_free (rdata);
        }

    // HEAD is sent, try to get data from cache
    } else {
        fileio_read_get_buf (rdata);
    }
}
/*}}}*/

/*{{{ fileio_simple_upload*/
typedef struct {
    gchar *fname;
    struct evbuffer *write_buf;
    FileIO_simple_on_upload_cb on_upload_cb;
    gpointer ctx;
    mode_t mode;
} FileIOSimpleUpload;

static void fileio_simple_upload_destroy (FileIOSimpleUpload *fsim)
{
    evbuffer_free (fsim->write_buf);
    g_free (fsim->fname);
    g_free (fsim);
}

static void fileio_simple_upload_on_sent_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileIOSimpleUpload *fsim = (FileIOSimpleUpload *) ctx;

    http_connection_release (con);

    fsim->on_upload_cb (fsim->ctx, success);

    fileio_simple_upload_destroy (fsim);
}

static void fileio_simple_upload_on_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileIOSimpleUpload *fsim = (FileIOSimpleUpload *) ctx;
    time_t t;
    gchar str[10];
    char time_str[50];
    gboolean res;

    LOG_debug (FIO_LOG, CON_H"Uploading data. Size: %zu", con, evbuffer_get_length (fsim->write_buf));

    http_connection_acquire (con);

    snprintf (str, sizeof (str), "%d", fsim->mode);

    http_connection_add_output_header (con, "x-amz-storage-class", conf_get_string (application_get_conf (con->app), "s3.storage_type"));
    http_connection_add_output_header (con, "x-amz-meta-mode", str);

    t = time (NULL);
    if (strftime (time_str, sizeof (time_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&t))) {
        http_connection_add_output_header (con, "x-amz-meta-date", time_str);
    }

    res = http_connection_make_request (con,
        fsim->fname, "PUT", fsim->write_buf, TRUE, NULL,
        fileio_simple_upload_on_sent_cb,
        fsim
    );

    if (!res) {
        LOG_err (FIO_LOG, CON_H"Failed to create HTTP request !", con);
        http_connection_release (con);
        fsim->on_upload_cb (fsim->ctx, FALSE);
        fileio_simple_upload_destroy (fsim);
        return;
    }
}

void fileio_simple_upload (Application *app, const gchar *fname, const char *str, mode_t mode, FileIO_simple_on_upload_cb on_upload_cb, gpointer ctx)
{
    FileIOSimpleUpload *fsim;

    fsim = g_new0 (FileIOSimpleUpload, 1);
    fsim->write_buf = evbuffer_new ();
    evbuffer_add (fsim->write_buf, str, strlen (str));
    fsim->fname = g_strdup_printf ("/%s", fname);
    fsim->on_upload_cb = on_upload_cb;
    fsim->ctx = ctx;
    fsim->mode = mode;

    if (!client_pool_get_client (application_get_write_client_pool (app),
        fileio_simple_upload_on_con_cb, fsim)) {
        LOG_err (FIO_LOG, "Failed to get HTTP client !");
        fsim->on_upload_cb (ctx, FALSE);
        fileio_simple_upload_destroy (fsim);
    }
}
/*}}}*/

/*{{{ fileio_simple_download*/
typedef struct {
    gchar *fname;
    FileIO_simple_on_download_cb on_download_cb;
    gpointer ctx;
} FileIOSimpleDownload;

static void fileio_simple_download_destroy (FileIOSimpleDownload *fsim)
{
    g_free (fsim->fname);
    g_free (fsim);
}

static void fileio_simple_download_on_sent_cb (HttpConnection *con, void *ctx, gboolean success,
    G_GNUC_UNUSED const gchar *buf, G_GNUC_UNUSED size_t buf_len,
    G_GNUC_UNUSED struct evkeyvalq *headers)
{
    FileIOSimpleDownload *fsim = (FileIOSimpleDownload *) ctx;

    http_connection_release (con);

    fsim->on_download_cb (fsim->ctx, success, buf, buf_len);

    fileio_simple_download_destroy (fsim);
}

static void fileio_simple_download_on_con_cb (gpointer client, gpointer ctx)
{
    HttpConnection *con = (HttpConnection *) client;
    FileIOSimpleDownload *fsim = (FileIOSimpleDownload *) ctx;
    gboolean res;

    LOG_debug (FIO_LOG, CON_H"Downloading data.", con);

    http_connection_acquire (con);

    http_connection_add_output_header (con, "x-amz-storage-class", conf_get_string (application_get_conf (con->app), "s3.storage_type"));

    res = http_connection_make_request (con,
        fsim->fname, "GET", NULL, TRUE, NULL,
        fileio_simple_download_on_sent_cb,
        fsim
    );

    if (!res) {
        LOG_err (FIO_LOG, CON_H"Failed to create HTTP request !", con);
        http_connection_release (con);
        fsim->on_download_cb (fsim->ctx, FALSE, NULL, 0);
        fileio_simple_download_destroy (fsim);
        return;
    }
}

void fileio_simple_download (Application *app, const gchar *fname, FileIO_simple_on_download_cb on_download_cb, gpointer ctx)
{
    FileIOSimpleDownload *fsim;

    fsim = g_new0 (FileIOSimpleDownload, 1);
    fsim->ctx = ctx;
    fsim->on_download_cb = on_download_cb;
    fsim->fname = g_strdup_printf ("/%s", fname);

    if (!client_pool_get_client (application_get_read_client_pool (app),
        fileio_simple_download_on_con_cb, fsim)) {
        LOG_err (FIO_LOG, "Failed to get HTTP client !");
        fsim->on_download_cb (ctx, FALSE, NULL, 0);
        fileio_simple_download_destroy (fsim);
    }

}
/*}}}*/
