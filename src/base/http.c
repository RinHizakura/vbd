// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Base image behind an HTTP(S) URL, read with range requests through
 * libcurl. The validator is the ETag, or Last-Modified if there is none.
 */
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

#include "base.h"

struct http {
	CURL *curl;
	char *url;
	pthread_mutex_t lock;	/* a CURL easy handle serves one request at a time */
};

/* Where a GET lands its body. */
struct sink {
	uint8_t *buf;
	size_t len, got;
};

/* Where HEAD collects the validator. */
struct tag {
	char *buf;
	size_t len;
	int have_etag;
};

static pthread_once_t global_once = PTHREAD_ONCE_INIT;

static void global_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

static int curl_errno(CURLcode rc, CURL *curl)
{
	long code = 0;

	switch (rc) {
	case CURLE_OK:
		return 0;
	case CURLE_COULDNT_CONNECT:
	case CURLE_COULDNT_RESOLVE_HOST:
		return -ECONNREFUSED;
	case CURLE_HTTP_RETURNED_ERROR:
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		return code == 404 ? -ENOENT : -EIO;
	default:
		return -EIO;
	}
}

/* Copy the value of a "Name: value" header line, trimmed, into @out. */
static void header_value(const char *line, size_t n, const char *name, char *out, size_t out_len)
{
	size_t k = strlen(name);

	if (n < k || strncasecmp(line, name, k))
		return;
	line += k; n -= k;
	while (n && isspace((unsigned char)*line)) {
		line++; n--;
	}
	while (n && isspace((unsigned char)line[n - 1]))
		n--;
	if (n >= out_len)
		n = out_len - 1;
	memcpy(out, line, n);
	out[n] = '\0';
}

static size_t on_header(char *line, size_t size, size_t n, void *arg)
{
	struct tag *t = arg;

	n *= size;
	if (!t->have_etag) {
		header_value(line, n, "ETag:", t->buf, t->len);
		if (t->buf[0])
			t->have_etag = 1;
		else
			header_value(line, n, "Last-Modified:", t->buf, t->len);
	}
	return n;
}

static size_t on_body(char *data, size_t size, size_t n, void *arg)
{
	struct sink *s = arg;

	n *= size;
	if (s->got + n > s->len)
		return 0;	/* server sent more than the range: abort */
	memcpy(s->buf + s->got, data, n);
	s->got += n;
	return n;
}

/* HEAD: size and validator. No Content-Length means the volume cannot be sized. */
static int http_head(const char *url, uint64_t *size, char *tag, size_t tag_len)
{
	struct tag t = { .buf = tag, .len = tag_len };
	curl_off_t cl = -1;
	CURL *curl;
	CURLcode rc;
	int ret;

	pthread_once(&global_once, global_init);
	tag[0] = '\0';
	curl = curl_easy_init();
	if (!curl)
		return -ENOMEM;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
	rc = curl_easy_perform(curl);
	ret = curl_errno(rc, curl);
	if (!ret) {
		curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
		if (cl < 0)
			ret = -EPROTO;	/* no Content-Length: cannot size the volume */
		else
			*size = cl;
	}
	curl_easy_cleanup(curl);
	return ret;
}

static void http_close(void *h);

static void *http_open(const char *url)
{
	struct http *h = calloc(1, sizeof(*h));

	pthread_once(&global_once, global_init);
	if (!h)
		return NULL;
	h->url = strdup(url);
	h->curl = curl_easy_init();
	if (!h->url || !h->curl) {
		http_close(h);
		errno = ENOMEM;
		return NULL;
	}
	pthread_mutex_init(&h->lock, NULL);
	return h;
}

static void http_close(void *arg)
{
	struct http *h = arg;

	if (!h)
		return;
	if (h->curl)
		curl_easy_cleanup(h->curl);
	free(h->url);
	free(h);
}

static int http_read(void *arg, uint64_t off, void *buf, size_t len)
{
	struct http *h = arg;
	struct sink s = { .buf = buf, .len = len };
	char range[64];
	long code = 0;
	CURLcode rc;
	int ret;

	snprintf(range, sizeof(range), "%llu-%llu", (unsigned long long)off,
		 (unsigned long long)(off + len - 1));

	pthread_mutex_lock(&h->lock);
	curl_easy_reset(h->curl);
	curl_easy_setopt(h->curl, CURLOPT_URL, h->url);
	curl_easy_setopt(h->curl, CURLOPT_RANGE, range);
	curl_easy_setopt(h->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h->curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(h->curl, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(h->curl, CURLOPT_WRITEDATA, &s);
	rc = curl_easy_perform(h->curl);
	ret = curl_errno(rc, h->curl);
	if (!ret) {
		curl_easy_getinfo(h->curl, CURLINFO_RESPONSE_CODE, &code);
		if (code != 206)
			ret = -EPROTO;	/* server ignored the Range header */
	}
	pthread_mutex_unlock(&h->lock);

	if (!ret && s.got < len)
		memset(s.buf + s.got, 0, len - s.got);	/* short at end of resource */
	return ret;
}

static int http_probe(const char *url, char *stored, size_t stored_len,
		      uint64_t *size, char *tag, size_t tag_len)
{
	if (strlen(url) >= stored_len)
		return -ENAMETOOLONG;
	strcpy(stored, url);
	return http_head(url, size, tag, tag_len);
}

const struct base_ops base_http_ops = {
	.probe = http_probe,
	.open = http_open,
	.read = http_read,
	.close = http_close,
};
