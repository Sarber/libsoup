/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-message-io.c: HTTP message I/O
 *
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "soup-connection.h"
#include "soup-message.h"
#include "soup-message-private.h"
#include "soup-message-queue.h"
#include "soup-misc.h"
#include "soup-output-stream.h"
#include "soup-socket.h"

typedef enum {
	SOUP_MESSAGE_IO_CLIENT,
	SOUP_MESSAGE_IO_SERVER
} SoupMessageIOMode;

typedef enum {
	SOUP_MESSAGE_IO_STATE_NOT_STARTED,
	SOUP_MESSAGE_IO_STATE_HEADERS,
	SOUP_MESSAGE_IO_STATE_BLOCKING,
	SOUP_MESSAGE_IO_STATE_BODY,
	SOUP_MESSAGE_IO_STATE_BODY_DATA,
	SOUP_MESSAGE_IO_STATE_CHUNK_SIZE,
	SOUP_MESSAGE_IO_STATE_CHUNK,
	SOUP_MESSAGE_IO_STATE_CHUNK_END,
	SOUP_MESSAGE_IO_STATE_TRAILERS,
	SOUP_MESSAGE_IO_STATE_FINISHING,
	SOUP_MESSAGE_IO_STATE_DONE
} SoupMessageIOState;

#define SOUP_MESSAGE_IO_STATE_ACTIVE(state) \
	(state != SOUP_MESSAGE_IO_STATE_NOT_STARTED && \
	 state != SOUP_MESSAGE_IO_STATE_BLOCKING && \
	 state != SOUP_MESSAGE_IO_STATE_DONE)

typedef struct {
	SoupMessageQueueItem *item;
	SoupMessageIOMode     mode;
	GCancellable         *cancellable;

	SoupSocket           *sock;
	SoupOutputStream     *ostream;
	GMainContext         *async_context;
	gboolean              blocking;

	SoupMessageIOState    read_state;
	SoupEncoding          read_encoding;
	GByteArray           *read_meta_buf;
	SoupMessageBody      *read_body;
	goffset               read_length;
	gboolean              read_eof_ok;

	gboolean              need_content_sniffed, need_got_chunk;
	SoupMessageBody      *sniff_data;

	SoupMessageIOState    write_state;
	SoupEncoding          write_encoding;
	GString              *write_buf;
	SoupMessageBody      *write_body;
	SoupBuffer           *write_chunk;
	goffset               write_body_offset;
	goffset               write_length;
	goffset               written;

	guint read_tag;
	GSource *write_source;
	GSource *unpause_source;
	gboolean paused;

	SoupMessageGetHeadersFn   get_headers_cb;
	SoupMessageParseHeadersFn parse_headers_cb;
	gpointer                  header_data;
	SoupMessageCompletionFn   completion_cb;
	gpointer                  completion_data;
} SoupMessageIOData;
	

/* Put these around callback invocation if there is code afterward
 * that depends on the IO having not been cancelled.
 */
#define dummy_to_make_emacs_happy {
#define SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK { gboolean cancelled; g_object_ref (msg);
#define SOUP_MESSAGE_IO_RETURN_IF_CANCELLED_OR_PAUSED cancelled = (priv->io_data != io); g_object_unref (msg); if (cancelled || io->paused) return; }
#define SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED(val) cancelled = (priv->io_data != io); g_object_unref (msg); if (cancelled || io->paused) return val; }

#define RESPONSE_BLOCK_SIZE 8192

void
soup_message_io_cleanup (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io;

	soup_message_io_stop (msg);

	io = priv->io_data;
	if (!io)
		return;
	priv->io_data = NULL;

	if (io->sock) {
		if (soup_socket_is_connected (io->sock)) {
			soup_output_stream_set_encoding (io->ostream,
							 SOUP_ENCODING_NONE);
		}
		g_object_unref (io->sock);
	}
	if (io->ostream)
		g_object_remove_weak_pointer (G_OBJECT (io->ostream), (gpointer *)&io->ostream);
	if (io->async_context)
		g_main_context_unref (io->async_context);
	if (io->item)
		soup_message_queue_item_unref (io->item);

	g_byte_array_free (io->read_meta_buf, TRUE);

	g_string_free (io->write_buf, TRUE);
	if (io->write_chunk)
		soup_buffer_free (io->write_chunk);

	if (io->sniff_data)
		soup_message_body_free (io->sniff_data);

	g_slice_free (SoupMessageIOData, io);
}

void
soup_message_io_stop (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;

	if (!io)
		return;

	if (io->read_tag) {
		g_signal_handler_disconnect (io->sock, io->read_tag);
		io->read_tag = 0;
	}
	if (io->write_source) {
		g_source_destroy (io->write_source);
		io->write_source = NULL;
	}

	if (io->unpause_source) {
		g_source_destroy (io->unpause_source);
		io->unpause_source = NULL;
	}

	if (io->read_state < SOUP_MESSAGE_IO_STATE_FINISHING)
		soup_socket_disconnect (io->sock);
}

#define SOUP_MESSAGE_IO_EOL            "\r\n"
#define SOUP_MESSAGE_IO_EOL_LEN        2

void
soup_message_io_finished (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	SoupMessageCompletionFn completion_cb = io->completion_cb;
	gpointer completion_data = io->completion_data;

	g_object_ref (msg);
	soup_message_io_cleanup (msg);
	if (completion_cb)
		completion_cb (msg, completion_data);
	g_object_unref (msg);
}

static void io_read (SoupSocket *sock, SoupMessage *msg);
static gboolean io_write (SoupOutputStream *stream, SoupMessage *msg);

static gboolean
request_is_idempotent (SoupMessage *msg)
{
	/* FIXME */
	return (msg->method == SOUP_METHOD_GET);
}

static void
io_error (SoupSocket *sock, SoupMessage *msg, GError *error)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;

	if (error && error->domain == G_TLS_ERROR) {
		soup_message_set_status_full (msg,
					      SOUP_STATUS_SSL_FAILED,
					      error->message);
	} else if (io->mode == SOUP_MESSAGE_IO_CLIENT &&
		   io->read_state <= SOUP_MESSAGE_IO_STATE_HEADERS &&
		   io->read_meta_buf->len == 0 &&
		   soup_connection_get_ever_used (io->item->conn) &&
		   !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) &&
		   request_is_idempotent (msg)) {
		/* Connection got closed, but we can safely try again */
		io->item->state = SOUP_MESSAGE_RESTARTING;
	} else if (!SOUP_STATUS_IS_TRANSPORT_ERROR (msg->status_code))
		soup_message_set_status (msg, SOUP_STATUS_IO_ERROR);

	if (error)
		g_error_free (error);

	soup_message_io_finished (msg);
}

static gboolean
io_handle_sniffing (SoupMessage *msg, gboolean done_reading)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	SoupBuffer *sniffed_buffer;
	char *sniffed_mime_type;
	GHashTable *params = NULL;

	if (!priv->sniffer)
		return TRUE;

	if (!io->sniff_data) {
		io->sniff_data = soup_message_body_new ();
		io->need_content_sniffed = TRUE;
	}

	if (io->need_content_sniffed) {
		if (io->sniff_data->length < priv->bytes_for_sniffing &&
		    !done_reading)
			return TRUE;

		io->need_content_sniffed = FALSE;
		sniffed_buffer = soup_message_body_flatten (io->sniff_data);
		sniffed_mime_type = soup_content_sniffer_sniff (priv->sniffer, msg, sniffed_buffer, &params);

		SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
		soup_message_content_sniffed (msg, sniffed_mime_type, params);
		g_free (sniffed_mime_type);
		if (params)
			g_hash_table_destroy (params);
		if (sniffed_buffer)
			soup_buffer_free (sniffed_buffer);
		SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
	}

	if (io->need_got_chunk) {
		io->need_got_chunk = FALSE;
		sniffed_buffer = soup_message_body_flatten (io->sniff_data);

		SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
		soup_message_got_chunk (msg, sniffed_buffer);
		soup_buffer_free (sniffed_buffer);
		SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
	}

	return TRUE;
}

static void
setup_write_source (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;

	io->write_source = g_pollable_output_stream_create_source (
		G_POLLABLE_OUTPUT_STREAM (io->ostream),
		NULL);
	g_source_set_callback (io->write_source, (GSourceFunc) io_write, msg, NULL);
	g_source_attach (io->write_source, io->async_context);
}

/* Reads data from io->sock into io->read_meta_buf. If @to_blank is
 * %TRUE, it reads up until a blank line ("CRLF CRLF" or "LF LF").
 * Otherwise, it reads up until a single CRLF or LF.
 *
 * This function is used to read metadata, and read_body_chunk() is
 * used to read the message body contents.
 *
 * read_metadata, read_body_chunk, and write_data all use the same
 * convention for return values: if they return %TRUE, it means
 * they've completely finished the requested read/write, and the
 * caller should move on to the next step. If they return %FALSE, it
 * means that either (a) the socket returned SOUP_SOCKET_WOULD_BLOCK,
 * so the caller should give up for now and wait for the socket to
 * emit a signal, or (b) the socket returned an error, and io_error()
 * was called to process it and cancel the I/O. So either way, if the
 * function returns %FALSE, the caller should return immediately.
 */
static gboolean
read_metadata (SoupMessage *msg, gboolean to_blank)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	SoupSocketIOStatus status;
	guchar read_buf[RESPONSE_BLOCK_SIZE];
	gsize nread;
	gboolean got_lf;
	GError *error = NULL;

	while (1) {
		status = soup_socket_read_until (io->sock, read_buf,
						 sizeof (read_buf),
						 "\n", 1, &nread, &got_lf,
						 io->cancellable, &error);
		switch (status) {
		case SOUP_SOCKET_OK:
			g_byte_array_append (io->read_meta_buf, read_buf, nread);
			break;

		case SOUP_SOCKET_EOF:
			/* More lame server handling... deal with
			 * servers that don't send the final chunk.
			 */
			if (io->read_state == SOUP_MESSAGE_IO_STATE_CHUNK_SIZE &&
			    io->read_meta_buf->len == 0) {
				g_byte_array_append (io->read_meta_buf,
						     (guchar *)"0\r\n", 3);
				got_lf = TRUE;
				break;
			} else if (io->read_state == SOUP_MESSAGE_IO_STATE_TRAILERS &&
				   io->read_meta_buf->len == 0) {
				g_byte_array_append (io->read_meta_buf,
						     (guchar *)"\r\n", 2);
				got_lf = TRUE;
				break;
			}
			/* else fall through */

		case SOUP_SOCKET_ERROR:
			io_error (io->sock, msg, error);
			return FALSE;

		case SOUP_SOCKET_WOULD_BLOCK:
			return FALSE;
		}

		if (got_lf) {
			if (!to_blank)
				break;
			if (nread == 1 &&
			    !strncmp ((char *)io->read_meta_buf->data +
				      io->read_meta_buf->len - 2,
				      "\n\n", 2))
				break;
			else if (nread == 2 &&
				 !strncmp ((char *)io->read_meta_buf->data +
					   io->read_meta_buf->len - 3,
					   "\n\r\n", 3))
				break;
		}
	}

	return TRUE;
}

static SoupBuffer *
content_decode_one (SoupBuffer *buf, GConverter *converter, GError **error)
{
	gsize outbuf_length, outbuf_used, outbuf_cur, input_used, input_cur;
	char *outbuf;
	GConverterResult result;

	outbuf_length = MAX (buf->length * 2, 1024);
	outbuf = g_malloc (outbuf_length);
	outbuf_cur = input_cur = 0;

	do {
		result = g_converter_convert (
			converter,
			buf->data + input_cur, buf->length - input_cur,
			outbuf + outbuf_cur, outbuf_length - outbuf_cur,
			0, &input_used, &outbuf_used, error);
		input_cur += input_used;
		outbuf_cur += outbuf_used;

		if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NO_SPACE) ||
		    (!*error && outbuf_cur == outbuf_length)) {
			g_clear_error (error);
			outbuf_length *= 2;
			outbuf = g_realloc (outbuf, outbuf_length);
		} else if (*error) {
			/* GZlibDecompressor can't ever return
			 * G_IO_ERROR_PARTIAL_INPUT unless we pass it
			 * input_length = 0, which we don't. Other
			 * converters might of course, so eventually
			 * this code needs to be rewritten to deal
			 * with that.
			 */
			g_free (outbuf);
			return NULL;
		}
	} while (input_cur < buf->length && result != G_CONVERTER_FINISHED);

	if (outbuf_cur)
		return soup_buffer_new (SOUP_MEMORY_TAKE, outbuf, outbuf_cur);
	else {
		g_free (outbuf);
		return NULL;
	}
}

static SoupBuffer *
content_decode (SoupMessage *msg, SoupBuffer *buf)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	GConverter *decoder;
	SoupBuffer *decoded;
	GError *error = NULL;
	GSList *d;

	for (d = priv->decoders; d; d = d->next) {
		decoder = d->data;

		decoded = content_decode_one (buf, decoder, &error);
		if (error) {
			if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED))
				g_warning ("Content-Decoding error: %s\n", error->message);
			g_error_free (error);

			soup_message_set_flags (msg, priv->msg_flags & ~SOUP_MESSAGE_CONTENT_DECODED);
			break;
		}
		if (buf)
			soup_buffer_free (buf);

		if (decoded)
			buf = decoded;
		else
			return NULL;
	}

	return buf;
}

/* Reads as much message body data as is available on io->sock (but no
 * further than the end of the current message body or chunk). On a
 * successful read, emits "got_chunk" (possibly multiple times), and
 * (unless told not to) appends the chunk to io->read_body.
 *
 * See the note at read_metadata() for an explanation of the return
 * value.
 */
static gboolean
read_body_chunk (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	SoupSocketIOStatus status;
	guchar *stack_buf = NULL;
	gsize len;
	gboolean read_to_eof = (io->read_encoding == SOUP_ENCODING_EOF);
	gsize nread;
	GError *error = NULL;
	SoupBuffer *buffer;

	if (!io_handle_sniffing (msg, FALSE))
		return FALSE;

	while (read_to_eof || io->read_length > 0) {
		if (priv->chunk_allocator) {
			buffer = priv->chunk_allocator (msg, io->read_length, priv->chunk_allocator_data);
			if (!buffer) {
				soup_message_io_pause (msg);
				return FALSE;
			}
		} else {
			if (!stack_buf)
				stack_buf = alloca (RESPONSE_BLOCK_SIZE);
			buffer = soup_buffer_new (SOUP_MEMORY_TEMPORARY,
						  stack_buf,
						  RESPONSE_BLOCK_SIZE);
		}

		if (read_to_eof)
			len = buffer->length;
		else
			len = MIN (buffer->length, io->read_length);

		status = soup_socket_read (io->sock,
					   (guchar *)buffer->data, len,
					   &nread, io->cancellable, &error);

		if (status == SOUP_SOCKET_OK && nread) {
			buffer->length = nread;
			io->read_length -= nread;

			buffer = content_decode (msg, buffer);
			if (!buffer)
				continue;

			soup_message_body_got_chunk (io->read_body, buffer);

			if (io->need_content_sniffed) {
				soup_message_body_append_buffer (io->sniff_data, buffer);
				soup_buffer_free (buffer);
				io->need_got_chunk = TRUE;
				if (!io_handle_sniffing (msg, FALSE))
					return FALSE;
				continue;
			}

			SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
			soup_message_got_chunk (msg, buffer);
			soup_buffer_free (buffer);
			SOUP_MESSAGE_IO_RETURN_VAL_IF_CANCELLED_OR_PAUSED (FALSE);
			continue;
		}

		soup_buffer_free (buffer);
		switch (status) {
		case SOUP_SOCKET_OK:
			break;

		case SOUP_SOCKET_EOF:
			if (io->read_eof_ok) {
				io->read_length = 0;
				return TRUE;
			}
			/* else fall through */

		case SOUP_SOCKET_ERROR:
			io_error (io->sock, msg, error);
			return FALSE;

		case SOUP_SOCKET_WOULD_BLOCK:
			return FALSE;
		}
	}

	return TRUE;
}

static gssize
do_write (SoupMessage *msg, const char *data, guint len)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	gssize nwrote;
	GError *error = NULL;

	if (!io->ostream) {
		io_error (io->sock, msg, NULL);
		return -1;
	}

	nwrote = soup_output_stream_write (io->ostream,
					   data + io->written,
					   len - io->written,
					   io->cancellable, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
		g_error_free (error);
		setup_write_source (msg);
		g_source_unref (io->write_source);
		return -1;
	} else if (error) {
		io_error (io->sock, msg, error);
		return -1;
	}

	io->written += nwrote;
	return nwrote;
}

static inline SoupMessageIOState
io_body_state (SoupEncoding encoding)
{
	if (encoding == SOUP_ENCODING_CHUNKED)
		return SOUP_MESSAGE_IO_STATE_CHUNK_SIZE;
	else
		return SOUP_MESSAGE_IO_STATE_BODY;
}

/*
 * There are two request/response formats: the basic request/response,
 * possibly with one or more unsolicited informational responses (such
 * as the WebDAV "102 Processing" response):
 *
 *     Client                            Server
 *      W:HEADERS  / R:NOT_STARTED    ->  R:HEADERS  / W:NOT_STARTED
 *      W:BODY     / R:NOT_STARTED    ->  R:BODY     / W:NOT_STARTED
 *     [W:DONE     / R:HEADERS (1xx)  <-  R:DONE     / W:HEADERS (1xx) ...]
 *      W:DONE     / R:HEADERS        <-  R:DONE     / W:HEADERS
 *      W:DONE     / R:BODY           <-  R:DONE     / W:BODY
 *      W:DONE     / R:DONE               R:DONE     / W:DONE
 *     
 * and the "Expect: 100-continue" request/response, with the client
 * blocking halfway through its request, and then either continuing or
 * aborting, depending on the server response:
 *
 *     Client                            Server
 *      W:HEADERS  / R:NOT_STARTED    ->  R:HEADERS  / W:NOT_STARTED
 *      W:BLOCKING / R:HEADERS        <-  R:BLOCKING / W:HEADERS
 *     [W:BODY     / R:BLOCKING       ->  R:BODY     / W:BLOCKING]
 *     [W:DONE     / R:HEADERS        <-  R:DONE     / W:HEADERS]
 *      W:DONE     / R:BODY           <-  R:DONE     / W:BODY
 *      W:DONE     / R:DONE               R:DONE     / W:DONE
 */

static gboolean
io_write (SoupOutputStream *stream, SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	SoupBuffer *chunk;
	gssize nwrote;

	if (io->write_source) {
		g_source_destroy (io->write_source);
		io->write_source = NULL;
	}

	g_object_ref (msg);

 write_more:
	switch (io->write_state) {
	case SOUP_MESSAGE_IO_STATE_NOT_STARTED:
		goto out;


	case SOUP_MESSAGE_IO_STATE_HEADERS:
		if (!io->write_buf->len) {
			io->get_headers_cb (msg, io->write_buf,
					    &io->write_encoding,
					    io->header_data);
			if (!io->write_buf->len) {
				soup_message_io_pause (msg);
				goto out;
			}
		}

		while (io->written < io->write_buf->len) {
			nwrote = do_write (msg,
					   io->write_buf->str + io->written,
					   io->write_buf->len - io->written);
			if (nwrote == -1)
				goto out;
			io->written += nwrote;
		}

		io->written = 0;
		g_string_truncate (io->write_buf, 0);

		if (io->write_encoding == SOUP_ENCODING_CONTENT_LENGTH) {
			SoupMessageHeaders *hdrs =
				(io->mode == SOUP_MESSAGE_IO_CLIENT) ?
				msg->request_headers : msg->response_headers;
			io->write_length = soup_message_headers_get_content_length (hdrs);
		}

		soup_output_stream_set_encoding (io->ostream, io->write_encoding);

		if (io->mode == SOUP_MESSAGE_IO_SERVER &&
		    SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
			if (msg->status_code == SOUP_STATUS_CONTINUE) {
				/* Stop and wait for the body now */
				io->write_state =
					SOUP_MESSAGE_IO_STATE_BLOCKING;
				io->read_state = io_body_state (io->read_encoding);
			} else {
				/* We just wrote a 1xx response
				 * header, so stay in STATE_HEADERS.
				 * (The caller will pause us from the
				 * wrote_informational callback if he
				 * is not ready to send the final
				 * response.)
				 */
			}
		} else if (io->mode == SOUP_MESSAGE_IO_CLIENT &&
			   soup_message_headers_get_expectations (msg->request_headers) & SOUP_EXPECTATION_CONTINUE) {
			/* Need to wait for the Continue response */
			io->write_state = SOUP_MESSAGE_IO_STATE_BLOCKING;
			io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;
		} else {
			io->write_state = SOUP_MESSAGE_IO_STATE_BODY;

			/* If the client was waiting for a Continue
			 * but we sent something else, then they're
			 * now done writing.
			 */
			if (io->mode == SOUP_MESSAGE_IO_SERVER &&
			    io->read_state == SOUP_MESSAGE_IO_STATE_BLOCKING)
				io->read_state = SOUP_MESSAGE_IO_STATE_FINISHING;
		}

		if (SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
			soup_message_wrote_informational (msg);
			soup_message_cleanup_response (msg);
		} else
			soup_message_wrote_headers (msg);
		break;


	case SOUP_MESSAGE_IO_STATE_BLOCKING:
		io_read (io->sock, msg);

		/* If io_read reached a point where we could write
		 * again, it would have recursively called io_write.
		 * So (a) we don't need to try to keep writing, and
		 * (b) we can't anyway, because msg may have been
		 * destroyed.
		 */
		goto out;


	case SOUP_MESSAGE_IO_STATE_BODY:
		if (!io->write_length &&
		    io->write_encoding != SOUP_ENCODING_EOF &&
		    io->write_encoding != SOUP_ENCODING_CHUNKED) {
		wrote_body:
			io->write_state = SOUP_MESSAGE_IO_STATE_FINISHING;
			soup_message_wrote_body (msg);
			break;
		}

		if (!io->write_chunk) {
			io->write_chunk = soup_message_body_get_chunk (io->write_body, io->write_body_offset);
			if (!io->write_chunk) {
				soup_message_io_pause (msg);
				goto out;
			}
			if (io->write_encoding == SOUP_ENCODING_EOF) {
				if (!io->write_chunk->length)
					goto wrote_body;
			} else if (io->write_encoding == SOUP_ENCODING_CHUNKED) {
				io->write_length = io->write_chunk->length;
			} else if (io->write_chunk->length > io->write_length) {
				/* App is trying to write more than it
				 * claimed it would; we have to truncate.
				 */
				SoupBuffer *truncated =
					soup_buffer_new_subbuffer (io->write_chunk,
								   0, io->write_length);
				soup_buffer_free (io->write_chunk);
				io->write_chunk = truncated;
			}
		}

		nwrote = do_write (msg, io->write_chunk->data + io->written,
				   io->write_chunk->length - io->written);
		if (nwrote == -1)
			goto out;

		chunk = soup_buffer_new (SOUP_MEMORY_TEMPORARY,
					 io->write_chunk->data + io->written,
					 nwrote);
		io->written += nwrote;
		if (io->write_length)
			io->write_length -= nwrote;

		io->write_state = SOUP_MESSAGE_IO_STATE_BODY_DATA;
		soup_message_wrote_body_data (msg, chunk);
		soup_buffer_free (chunk);
		break;

	case SOUP_MESSAGE_IO_STATE_BODY_DATA:
		if (io->written < io->write_chunk->length)
			break;

		io->written = 0;
		if (io->write_chunk->length == 0)
			goto wrote_body;

		if (io->mode == SOUP_MESSAGE_IO_SERVER ||
		    priv->msg_flags & SOUP_MESSAGE_CAN_REBUILD)
			soup_message_body_wrote_chunk (io->write_body, io->write_chunk);
		io->write_body_offset += io->write_chunk->length;
		soup_buffer_free (io->write_chunk);
		io->write_chunk = NULL;

		io->write_state = SOUP_MESSAGE_IO_STATE_BODY;
		soup_message_wrote_chunk (msg);
		break;

	case SOUP_MESSAGE_IO_STATE_FINISHING:
		if (io->write_source) {
			g_source_destroy (io->write_source);
			io->write_source = NULL;
		}
		io->write_state = SOUP_MESSAGE_IO_STATE_DONE;

		if (io->mode == SOUP_MESSAGE_IO_CLIENT) {
			io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;
			io_read (io->sock, msg);
		} else
			soup_message_io_finished (msg);
		goto out;


	case SOUP_MESSAGE_IO_STATE_DONE:
	default:
		g_warn_if_reached ();
	}

	if (priv->io_data == io && !io->paused)
		goto write_more;

 out:
	g_object_unref (msg);
	return FALSE;
}

static void
io_read (SoupSocket *sock, SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;
	guint status;

 read_more:
	switch (io->read_state) {
	case SOUP_MESSAGE_IO_STATE_NOT_STARTED:
		return;


	case SOUP_MESSAGE_IO_STATE_HEADERS:
		if (!read_metadata (msg, TRUE))
			return;

		/* We need to "rewind" io->read_meta_buf back one line.
		 * That SHOULD be two characters (CR LF), but if the
		 * web server was stupid, it might only be one.
		 */
		if (io->read_meta_buf->len < 3 ||
		    io->read_meta_buf->data[io->read_meta_buf->len - 2] == '\n')
			io->read_meta_buf->len--;
		else
			io->read_meta_buf->len -= 2;
		io->read_meta_buf->data[io->read_meta_buf->len] = '\0';
		status = io->parse_headers_cb (msg, (char *)io->read_meta_buf->data,
					       io->read_meta_buf->len,
					       &io->read_encoding,
					       io->header_data);
		g_byte_array_set_size (io->read_meta_buf, 0);

		if (status != SOUP_STATUS_OK) {
			/* Either we couldn't parse the headers, or they
			 * indicated something that would mean we wouldn't
			 * be able to parse the body. (Eg, unknown
			 * Transfer-Encoding.). Skip the rest of the
			 * reading, and make sure the connection gets
			 * closed when we're done.
			 */
			soup_message_set_status (msg, status);
			soup_message_headers_append (msg->request_headers,
						     "Connection", "close");
			io->read_state = SOUP_MESSAGE_IO_STATE_FINISHING;
			break;
		}

		if (io->read_encoding == SOUP_ENCODING_EOF)
			io->read_eof_ok = TRUE;

		if (io->read_encoding == SOUP_ENCODING_CONTENT_LENGTH) {
			SoupMessageHeaders *hdrs =
				(io->mode == SOUP_MESSAGE_IO_CLIENT) ?
				msg->response_headers : msg->request_headers;
			io->read_length = soup_message_headers_get_content_length (hdrs);

			if (io->mode == SOUP_MESSAGE_IO_CLIENT &&
			    !soup_message_is_keepalive (msg)) {
				/* Some servers suck and send
				 * incorrect Content-Length values, so
				 * allow EOF termination in this case
				 * (iff the message is too short) too.
				 */
				io->read_eof_ok = TRUE;
			}
		}

		if (io->mode == SOUP_MESSAGE_IO_CLIENT &&
		    SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
			if (msg->status_code == SOUP_STATUS_CONTINUE &&
			    io->write_state == SOUP_MESSAGE_IO_STATE_BLOCKING) {
				/* Pause the reader, unpause the writer */
				io->read_state =
					SOUP_MESSAGE_IO_STATE_BLOCKING;
				io->write_state =
					SOUP_MESSAGE_IO_STATE_BODY;
			} else {
				/* Just stay in HEADERS */
				io->read_state = SOUP_MESSAGE_IO_STATE_HEADERS;
			}
		} else if (io->mode == SOUP_MESSAGE_IO_SERVER &&
			   soup_message_headers_get_expectations (msg->request_headers) & SOUP_EXPECTATION_CONTINUE) {
			/* The client requested a Continue response. The
			 * got_headers handler may change this to something
			 * else though.
			 */
			soup_message_set_status (msg, SOUP_STATUS_CONTINUE);
			io->write_state = SOUP_MESSAGE_IO_STATE_HEADERS;
			io->read_state = SOUP_MESSAGE_IO_STATE_BLOCKING;
		} else {
			io->read_state = io_body_state (io->read_encoding);

			/* If the client was waiting for a Continue
			 * but got something else, then it's done
			 * writing.
			 */
			if (io->mode == SOUP_MESSAGE_IO_CLIENT &&
			    io->write_state == SOUP_MESSAGE_IO_STATE_BLOCKING)
				io->write_state = SOUP_MESSAGE_IO_STATE_FINISHING;
		}

		if (io->mode == SOUP_MESSAGE_IO_CLIENT &&
		    SOUP_STATUS_IS_INFORMATIONAL (msg->status_code)) {
			SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
			soup_message_got_informational (msg);
			soup_message_cleanup_response (msg);
			SOUP_MESSAGE_IO_RETURN_IF_CANCELLED_OR_PAUSED;
		} else {
			SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
			soup_message_got_headers (msg);
			SOUP_MESSAGE_IO_RETURN_IF_CANCELLED_OR_PAUSED;
		}
		break;


	case SOUP_MESSAGE_IO_STATE_BLOCKING:
		io_write (io->ostream, msg);

		/* As in the io_write case, we *must* return here. */
		return;


	case SOUP_MESSAGE_IO_STATE_BODY:
		if (!read_body_chunk (msg))
			return;

	got_body:
		if (!io_handle_sniffing (msg, TRUE)) {
			/* If the message was paused (as opposed to
			 * cancelled), we need to make sure we wind up
			 * back here when it's unpaused, even if it
			 * was doing a chunked or EOF-terminated read
			 * before.
			 */
			if (io == priv->io_data) {
				io->read_state = SOUP_MESSAGE_IO_STATE_BODY;
				io->read_encoding = SOUP_ENCODING_CONTENT_LENGTH;
				io->read_length = 0;
			}
			return;
		}

		io->read_state = SOUP_MESSAGE_IO_STATE_FINISHING;

		SOUP_MESSAGE_IO_PREPARE_FOR_CALLBACK;
		soup_message_got_body (msg);
		SOUP_MESSAGE_IO_RETURN_IF_CANCELLED_OR_PAUSED;
		break;


	case SOUP_MESSAGE_IO_STATE_CHUNK_SIZE:
		if (!read_metadata (msg, FALSE))
			return;

		io->read_length = strtoul ((char *)io->read_meta_buf->data, NULL, 16);
		g_byte_array_set_size (io->read_meta_buf, 0);

		if (io->read_length > 0)
			io->read_state = SOUP_MESSAGE_IO_STATE_CHUNK;
		else
			io->read_state = SOUP_MESSAGE_IO_STATE_TRAILERS;
		break;


	case SOUP_MESSAGE_IO_STATE_CHUNK:
		if (!read_body_chunk (msg))
			return;

		io->read_state = SOUP_MESSAGE_IO_STATE_CHUNK_END;
		break;


	case SOUP_MESSAGE_IO_STATE_CHUNK_END:
		if (!read_metadata (msg, FALSE))
			return;

		g_byte_array_set_size (io->read_meta_buf, 0);
		io->read_state = SOUP_MESSAGE_IO_STATE_CHUNK_SIZE;
		break;


	case SOUP_MESSAGE_IO_STATE_TRAILERS:
		if (!read_metadata (msg, FALSE))
			return;

		if (io->read_meta_buf->len <= SOUP_MESSAGE_IO_EOL_LEN)
			goto got_body;

		/* FIXME: process trailers */
		g_byte_array_set_size (io->read_meta_buf, 0);
		break;


	case SOUP_MESSAGE_IO_STATE_FINISHING:
		if (io->read_tag) {
			g_signal_handler_disconnect (io->sock, io->read_tag);
			io->read_tag = 0;
		}
		io->read_state = SOUP_MESSAGE_IO_STATE_DONE;

		if (io->mode == SOUP_MESSAGE_IO_SERVER) {
			io->write_state = SOUP_MESSAGE_IO_STATE_HEADERS;
			io_write (io->ostream, msg);
		} else
			soup_message_io_finished (msg);
		return;


	case SOUP_MESSAGE_IO_STATE_DONE:
	default:
		g_return_if_reached ();
	}

	goto read_more;
}

static SoupMessageIOData *
new_iostate (SoupMessage *msg, SoupSocket *sock, SoupMessageIOMode mode,
	     SoupMessageGetHeadersFn get_headers_cb,
	     SoupMessageParseHeadersFn parse_headers_cb,
	     gpointer header_data,
	     SoupMessageCompletionFn completion_cb,
	     gpointer completion_data)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io;
	gboolean non_blocking;

	io = g_slice_new0 (SoupMessageIOData);
	io->mode = mode;
	io->get_headers_cb   = get_headers_cb;
	io->parse_headers_cb = parse_headers_cb;
	io->header_data      = header_data;
	io->completion_cb    = completion_cb;
	io->completion_data  = completion_data;

	io->sock = g_object_ref (sock);
	io->ostream = SOUP_OUTPUT_STREAM (soup_socket_get_output_stream (sock));
	if (io->ostream)
		g_object_add_weak_pointer (G_OBJECT (io->ostream), (gpointer *)&io->ostream);

	g_object_get (io->sock,
		      SOUP_SOCKET_FLAG_NONBLOCKING, &non_blocking,
		      SOUP_SOCKET_ASYNC_CONTEXT, &io->async_context,
		      NULL);
	io->blocking = !non_blocking;

	io->read_meta_buf    = g_byte_array_new ();
	io->write_buf        = g_string_new (NULL);

	io->read_tag  = g_signal_connect (io->sock, "readable",
					  G_CALLBACK (io_read), msg);

	io->read_state  = SOUP_MESSAGE_IO_STATE_NOT_STARTED;
	io->write_state = SOUP_MESSAGE_IO_STATE_NOT_STARTED;

	if (priv->io_data)
		soup_message_io_cleanup (msg);
	priv->io_data = io;
	return io;
}

void
soup_message_io_client (SoupMessageQueueItem *item,
			SoupMessageGetHeadersFn get_headers_cb,
			SoupMessageParseHeadersFn parse_headers_cb,
			gpointer header_data,
			SoupMessageCompletionFn completion_cb,
			gpointer completion_data)
{
	SoupMessageIOData *io;
	SoupSocket *sock = soup_connection_get_socket (item->conn);

	io = new_iostate (item->msg, sock, SOUP_MESSAGE_IO_CLIENT,
			  get_headers_cb, parse_headers_cb, header_data,
			  completion_cb, completion_data);

	io->item = item;
	soup_message_queue_item_ref (item);
	io->cancellable = item->cancellable;

	io->read_body       = item->msg->response_body;
	io->write_body      = item->msg->request_body;

	io->write_state     = SOUP_MESSAGE_IO_STATE_HEADERS;
	io_write (io->ostream, item->msg);
}

void
soup_message_io_server (SoupMessage *msg, SoupSocket *sock,
			SoupMessageGetHeadersFn get_headers_cb,
			SoupMessageParseHeadersFn parse_headers_cb,
			gpointer header_data,
			SoupMessageCompletionFn completion_cb,
			gpointer completion_data)
{
	SoupMessageIOData *io;

	io = new_iostate (msg, sock, SOUP_MESSAGE_IO_SERVER,
			  get_headers_cb, parse_headers_cb, header_data,
			  completion_cb, completion_data);

	io->read_body       = msg->request_body;
	io->write_body      = msg->response_body;

	io->read_state      = SOUP_MESSAGE_IO_STATE_HEADERS;
	io_read (sock, msg);
}

void  
soup_message_io_pause (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;

	g_return_if_fail (io != NULL);

	if (io->write_source) {
		g_source_destroy (io->write_source);
		io->write_source = NULL;
	}
	if (io->read_tag) {
		g_signal_handler_disconnect (io->sock, io->read_tag);
		io->read_tag = 0;
	}

	if (io->unpause_source) {
		g_source_destroy (io->unpause_source);
		io->unpause_source = NULL;
	}

	io->paused = TRUE;
}

static gboolean
io_unpause_internal (gpointer msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;

	g_return_val_if_fail (io != NULL, FALSE);
	io->unpause_source = NULL;
	io->paused = FALSE;

	if (io->write_source || io->read_tag)
		return FALSE;

	if (io->read_state != SOUP_MESSAGE_IO_STATE_DONE) {
		io->read_tag = g_signal_connect (io->sock, "readable",
						 G_CALLBACK (io_read), msg);
	}

	if (SOUP_MESSAGE_IO_STATE_ACTIVE (io->write_state))
		io_write (io->ostream, msg);
	else if (SOUP_MESSAGE_IO_STATE_ACTIVE (io->read_state))
		io_read (io->sock, msg);

	return FALSE;
}

void
soup_message_io_unpause (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);
	SoupMessageIOData *io = priv->io_data;

	g_return_if_fail (io != NULL);

	if (!io->blocking) {
		if (!io->unpause_source) {
			io->unpause_source = soup_add_completion (
				io->async_context, io_unpause_internal, msg);
		}
	} else
		io_unpause_internal (msg);
}

/**
 * soup_message_io_in_progress:
 * @msg: a #SoupMessage
 *
 * Tests whether or not I/O is currently in progress on @msg.
 *
 * Return value: whether or not I/O is currently in progress.
 **/
gboolean
soup_message_io_in_progress (SoupMessage *msg)
{
	SoupMessagePrivate *priv = SOUP_MESSAGE_GET_PRIVATE (msg);

	return priv->io_data != NULL;
}
