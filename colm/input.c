/*
 *  Copyright 2007-2011 Adrian Thurston <thurston@complang.org>
 */

/*  This file is part of Colm.
 *
 *  Colm is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 * 
 *  Colm is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with Colm; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */

#include <colm/input.h>
#include <colm/fsmrun.h>
#include <colm/pdarun.h>
#include <colm/debug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

RunBuf *newRunBuf()
{
	RunBuf *rb = (RunBuf*)malloc(sizeof(RunBuf));
	memset( rb, 0, sizeof(RunBuf) );
	return rb;
}

#define true 1
#define false 0

void initDynamicFuncs();
void initFileFuncs();
void initFdFuncs();

void initPatternFuncs();
void initReplFuncs();

struct SourceFuncs dynamicFuncs;
struct SourceFuncs fileFuncs;
struct SourceFuncs fdFuncs;

void initInputStream( InputStream *inputStream )
{
	memset( inputStream, 0, sizeof(InputStream) );

	/* FIXME: correct values here. */
	inputStream->line = 1;
	inputStream->column = 1;
	inputStream->byte = 0;
}

void initSourceStream( SourceStream *inputStream )
{
	/* FIXME: correct values here. */
	inputStream->line = 1;
	inputStream->column = 1;
	inputStream->byte = 0;
}

SourceStream *newInputStreamFile( FILE *file )
{
	SourceStream *is = (SourceStream*)malloc(sizeof(SourceStream));
	memset( is, 0, sizeof(SourceStream) );
	is->line = 1;
	is->column = 1;
	is->file = file;
	is->funcs = &fileFuncs;
	return is;
}

SourceStream *newInputStreamFd( long fd )
{
	SourceStream *is = (SourceStream*)malloc(sizeof(SourceStream));
	memset( is, 0, sizeof(SourceStream) );
	is->line = 1;
	is->column = 1;
	is->fd = fd;
	is->funcs = &fdFuncs;
	return is;
}

RunBuf *inputStreamHead( SourceStream *is )
{ 
	return is->queue;
}

RunBuf *inputStreamTail( SourceStream *is )
{
	return is->queueTail;
}

RunBuf *inputStreamPopHead( SourceStream *is )
{
	RunBuf *ret = is->queue;
	is->queue = is->queue->next;
	if ( is->queue == 0 )
		is->queueTail = 0;
	else
		is->queue->prev = 0;
	return ret;
}

RunBuf *inputStreamPopTail( SourceStream *is )
{
	RunBuf *ret = is->queueTail;
	is->queueTail = is->queue->prev;
	if ( is->queueTail == 0 )
		is->queue = 0;
	else
		is->queueTail->next = 0;
	return ret;
}

void inputStreamAppend( SourceStream *is, RunBuf *runBuf )
{
	if ( is->queue == 0 ) {
		runBuf->prev = runBuf->next = 0;
		is->queue = is->queueTail = runBuf;
	}
	else {
		is->queueTail->next = runBuf;
		runBuf->prev = is->queueTail;
		runBuf->next = 0;
		is->queueTail = runBuf;
	}
}

void inputStreamPrepend( SourceStream *is, RunBuf *runBuf )
{
	if ( is->queue == 0 ) {
		runBuf->prev = runBuf->next = 0;
		is->queue = is->queueTail = runBuf;
	}
	else {
		is->queue->prev = runBuf;
		runBuf->prev = 0;
		runBuf->next = is->queue;
		is->queue = runBuf;
	}
}


void initInputFuncs()
{
	initDynamicFuncs();
	initFileFuncs();
	initFdFuncs();
	initPatternFuncs();
	initReplFuncs();
}

/* 
 * Base run-time input streams.
 */

int inputStreamDynamicGetData( SourceStream *is, int skip, char *dest, int length, int *copied )
{
	int ret = 0;
	*copied = 0;

	/* Move over skip bytes. */
	RunBuf *buf = is->queue;
	while ( true ) {
		if ( buf == 0 ) {
			/* Got through the in-mem buffers without copying anything. */
			RunBuf *runBuf = newRunBuf();
			inputStreamPrepend( is, runBuf );
			int received = is->funcs->getDataImpl( is, runBuf->data, FSM_BUFSIZE );
			if ( received == 0 ) {
				ret = INPUT_EOD;
				break;
			}
			runBuf->length = received;

			int slen = received < length ? received : length;
			memcpy( dest, runBuf->data, slen );
			*copied = slen;
			ret = INPUT_DATA;
			break;
		}

		int avail = buf->length - buf->offset;

		/* Anything available in the current buffer. */
		if ( avail > 0 ) {
			/* The source data from the current buffer. */
			char *src = &buf->data[buf->offset];
			int slen = avail <= length ? avail : length;

			/* Need to skip? */
			if ( skip > 0 && slen <= skip ) {
				/* Skipping the the whole source. */
				skip -= slen;
			}
			else {
				/* Either skip is zero, or less than slen. Skip goes to zero.
				 * Some data left over, copy it. */
				src += skip;
				slen -= skip;
				skip = 0;

				memcpy( dest, src, slen ) ;
				*copied += slen;
				ret = INPUT_DATA;
				break;
			}
		}

		buf = buf->next;
	}

	return ret;
}

int inputStreamDynamicConsumeData( SourceStream *is, int length )
{
	debug( REALM_INPUT, "source consuming %ld bytes\n", length );

	while ( is->queue->offset == is->queue->length ) {
		RunBuf *runBuf = inputStreamPopHead( is );
		free( runBuf );
	}

	is->queue->offset += length;

	return length;
}

int inputStreamDynamicUndoConsumeData( SourceStream *is, const char *data, int length )
{
	debug( REALM_INPUT, "undoing consume of %ld bytes\n", length );

	RunBuf *newBuf = newRunBuf();
	newBuf->length = length;
	memcpy( newBuf->data, data, length );
	inputStreamPrepend( is, newBuf );

	return length;
}

int inputStreamDynamicGetDataRev( SourceStream *is, char *dest, int length )
{
	/* If there is any data in the rubuf queue then read that first. */
	if ( is->queueTail != 0 ) {
		long avail = is->queueTail->length - is->queueTail->offset;
		if ( length >= avail ) {
			memcpy( dest, &is->queueTail->data[is->queue->offset], avail );
			RunBuf *del = inputStreamPopTail(is);
			free(del);
			return avail;
		}
		else {
			memcpy( dest, &is->queueTail->data[is->queueTail->offset], length );
			is->queueTail->length -= length;
			return length;
		}
	}
	return 0;
}

Tree *inputStreamDynamicConsumeTree( SourceStream *is )
{
	if ( is->queue != 0 && (is->queue->type == RunBufTokenType || is->queue->type == RunBufIgnoreType) ) {
		RunBuf *runBuf = inputStreamPopHead( is );

		/* FIXME: using runbufs here for this is a poor use of memory. */
		Tree *tree = runBuf->tree;
		free(runBuf);
		return tree;
	}

	return 0;
}

void inputStreamDynamicPushText( SourceStream *is, const char *data, long length )
{
//	#ifdef COLM_LOG_PARSE
//	if ( colm_log_parse ) {
//		cerr << "readying fake push" << endl;
//	}
//	#endif

//	takeBackBuffered( inputStream );

	/* Create a new buffer for the data. This is the easy implementation.
	 * Something better is needed here. It puts a max on the amount of
	 * data that can be pushed back to the inputStream. */
	assert( length < FSM_BUFSIZE );

	RunBuf *newBuf = newRunBuf();
	newBuf->length = length;
	memcpy( newBuf->data, data, length );

	inputStreamPrepend( is, newBuf );
}

void inputStreamDynamicUndoConsumeTree( SourceStream *is, Tree *tree, int ignore )
{
//	#ifdef COLM_LOG_PARSE
//	if ( colm_log_parse ) {
//		cerr << "readying fake push" << endl;
//	}
//	#endif

//	takeBackBuffered( inputStream );

	/* Create a new buffer for the data. This is the easy implementation.
	 * Something better is needed here. It puts a max on the amount of
	 * data that can be pushed back to the inputStream. */
	RunBuf *newBuf = newRunBuf();
	newBuf->type = ignore ? RunBufIgnoreType : RunBufTokenType;
	newBuf->tree = tree;

	inputStreamPrepend( is, newBuf );
}

Tree *inputStreamDynamicUndoPrependData( SourceStream *is, int length )
{
	if ( is->queue->type == RunBufDataType ) {
		char tmp[length];
		int have = 0;
		while ( have < length ) {
			int res = 0;
			is->funcs->getData( is, 0, tmp, length-have, &res );
			have += res;
		}
		return 0;
	}
	else {
		/* FIXME: leak here. */
		RunBuf *rb = inputStreamPopHead( is );
		Tree *tree = rb->tree;
		free(rb);
		return tree;
	}
}

Tree *inputStreamDynamicUndoAppend( SourceStream *is, int length )
{
	if ( is->queueTail->type == RunBufDataType ) {
		char tmp[length];
		int have = 0;
		while ( have < length ) {
			int res = inputStreamDynamicGetDataRev( is, tmp, length-have );
			have += res;
		}
		return 0;
	}
	else {
		/* FIXME: leak here. */
		RunBuf *rb = inputStreamPopTail( is );
		Tree *tree = rb->tree;
		free(rb);
		return tree;
	}
}

void initDynamicFuncs()
{
	memset( &dynamicFuncs, 0, sizeof(struct SourceFuncs) );

	dynamicFuncs.getData = &inputStreamDynamicGetData;
	dynamicFuncs.consumeData = &inputStreamDynamicConsumeData;
	dynamicFuncs.undoConsumeData = &inputStreamDynamicUndoConsumeData;

	dynamicFuncs.consumeTree = &inputStreamDynamicConsumeTree;
	dynamicFuncs.undoConsumeTree = &inputStreamDynamicUndoConsumeTree;

	dynamicFuncs.undoPrependData = &inputStreamDynamicUndoPrependData;
	dynamicFuncs.undoAppendData = &inputStreamDynamicUndoAppend;
	dynamicFuncs.prependData = &inputStreamDynamicPushText;
}

/*
 * File
 */

int inputStreamFileGetDataImpl( SourceStream *is, char *dest, int length )
{
	debug( REALM_INPUT, "inputStreamFileGetDataImpl length = %ld\n", length );
	size_t res = fread( dest, 1, length, is->file );
	if ( res < (size_t) length ) {
		debug( REALM_INPUT, "setting later = true\n" );
		is->later = true;
	}
	return res;
}

void initFileFuncs()
{
	memcpy( &fileFuncs, &dynamicFuncs, sizeof(struct SourceFuncs) );
	fileFuncs.getDataImpl = &inputStreamFileGetDataImpl;
}

/*
 * FD
 */

int inputStreamFdGetDataImpl( SourceStream *is, char *dest, int length )
{
	long got = read( is->fd, dest, length );
	if ( got == 0 )
		is->later = true;
	return got;
}

void initFdFuncs()
{
	memcpy( &fdFuncs, &dynamicFuncs, sizeof(struct SourceFuncs) );
	fdFuncs.getDataImpl = &inputStreamFdGetDataImpl;
}

/*
 * InputStream struct, this wraps the list of input streams.
 */

static void inputStreamPrepend2( InputStream *is, RunBuf *runBuf )
{
	if ( is->queue == 0 ) {
		runBuf->prev = runBuf->next = 0;
		is->queue = is->queueTail = runBuf;
	}
	else {
		is->queue->prev = runBuf;
		runBuf->prev = 0;
		runBuf->next = is->queue;
		is->queue = runBuf;
	}
}

RunBuf *inputStreamPopHead2( InputStream *is )
{
	RunBuf *ret = is->queue;
	is->queue = is->queue->next;
	if ( is->queue == 0 )
		is->queueTail = 0;
	else
		is->queue->prev = 0;
	return ret;
}

static void inputStreamAppend2( InputStream *is, RunBuf *runBuf )
{
	if ( is->queue == 0 ) {
		runBuf->prev = runBuf->next = 0;
		is->queue = is->queueTail = runBuf;
	}
	else {
		is->queueTail->next = runBuf;
		runBuf->prev = is->queueTail;
		runBuf->next = 0;
		is->queueTail = runBuf;
	}
}

static RunBuf *inputStreamPopTail2( InputStream *is )
{
	RunBuf *ret = is->queueTail;
	is->queueTail = is->queue->prev;
	if ( is->queueTail == 0 )
		is->queue = 0;
	else
		is->queueTail->next = 0;
	return ret;
}

static int inputStreamDynamicGetDataRev2( InputStream *is, char *dest, int length )
{
	/* If there is any data in the rubuf queue then read that first. */
	if ( is->queueTail != 0 ) {
		long avail = is->queueTail->length - is->queueTail->offset;
		if ( length >= avail ) {
			memcpy( dest, &is->queueTail->data[is->queue->offset], avail );
			RunBuf *del = inputStreamPopTail2(is);
			free(del);
			return avail;
		}
		else {
			memcpy( dest, &is->queueTail->data[is->queueTail->offset], length );
			is->queueTail->length -= length;
			return length;
		}
	}
	return 0;
}

static int isSourceStream( InputStream *is )
{
	if ( is->queue != 0 && is->queue->type == RunBufSourceType )
		return true;
	return false;
}

void setEof( InputStream *is )
{
	debug( REALM_INPUT, "setting EOF in input stream\n" );
	is->eof = true;
}

void unsetEof( InputStream *is )
{
	if ( isSourceStream( is ) ) {
		Stream *stream = (Stream*)is->queue->tree;
		stream->in->eof = false;
	}
	else {
		is->eof = false;
	}
}

int getData( InputStream *is, int skip, char *dest, int length, int *copied )
{
	int ret = 0;
	*copied = 0;

	/* Move over skip bytes. */
	RunBuf *buf = is->queue;
	while ( true ) {
		if ( buf == 0 ) {
			/* Got through the in-mem buffers without copying anything. */
			ret = is->eof ? INPUT_EOF : INPUT_EOD;
			break;
		}

		if ( buf->type == RunBufSourceType ) {
			Stream *stream = (Stream*)buf->tree;
			int type = stream->in->funcs->getData( stream->in, skip, dest, length, copied );

			if ( type == INPUT_EOD && is->eof ) {
				ret = INPUT_EOF;
				break;
			}

			ret = type;
			break;
		}

		if ( buf->type == RunBufTokenType ) {
			ret = INPUT_TREE;
			break;
		}

		if ( buf->type == RunBufIgnoreType ) {
			ret = INPUT_IGNORE;
			break;
		}

		int avail = buf->length - buf->offset;

		/* Anything available in the current buffer. */
		if ( avail > 0 ) {
			/* The source data from the current buffer. */
			char *src = &buf->data[buf->offset];
			int slen = avail <= length ? avail : length;

			/* Need to skip? */
			if ( skip > 0 && slen <= skip ) {
				/* Skipping the the whole source. */
				skip -= slen;
			}
			else {
				/* Either skip is zero, or less than slen. Skip goes to zero.
				 * Some data left over, copy it. */
				src += skip;
				slen -= skip;
				skip = 0;

				memcpy( dest, src, slen ) ;
				*copied += slen;
				ret = INPUT_DATA;
				break;
			}
		}

		buf = buf->next;
	}

#if DEBUG
	switch ( ret ) {
		case INPUT_DATA:
			debug( REALM_INPUT, "get data: DATA copied: %d\n", *copied );
			break;
		case INPUT_EOD:
			debug( REALM_INPUT, "get data: EOD\n" );
			break;
		case INPUT_EOF:
			debug( REALM_INPUT, "get data: EOF\n" );
			break;
		case INPUT_TREE:
			debug( REALM_INPUT, "get data: TREE\n" );
			break;
		case INPUT_IGNORE:
			debug( REALM_INPUT, "get data: IGNORE\n" );
			break;
		case INPUT_LANG_EL:
			debug( REALM_INPUT, "get data: LANG_EL\n" );
			break;
	}
#endif

	return ret;
}

int consumeData( InputStream *is, int length )
{
	debug( REALM_INPUT, "consuming %d bytes\n", length );

	int consumed = 0;

	/* Move over skip bytes. */
	while ( true ) {
		RunBuf *buf = is->queue;

		if ( buf == 0 )
			break;

		if ( buf->type == RunBufSourceType ) {
			Stream *stream = (Stream*)buf->tree;
			int slen = stream->in->funcs->consumeData( stream->in, length );

			consumed += slen;
			length -= slen;
		}
		else if ( buf->type == RunBufTokenType )
			break;
		else if ( buf->type == RunBufIgnoreType )
			break;
		else {
			/* Anything available in the current buffer. */
			int avail = buf->length - buf->offset;
			if ( avail > 0 ) {
				/* The source data from the current buffer. */
				int slen = avail <= length ? avail : length;
				consumed += slen;
				length -= slen;
				buf->offset += slen;
			}
		}

		if ( length == 0 )
			break;

		free( inputStreamPopHead2( is ) );
	}

	return consumed;
}

int undoConsumeData( InputStream *is, const char *data, int length )
{
	if ( isSourceStream( is ) ) {
		Stream *stream = (Stream*)is->queue->tree;
		return stream->in->funcs->undoConsumeData( stream->in, data, length );
	}
	else {
		debug( REALM_INPUT, "undoing consume of %ld bytes\n", length );

		RunBuf *newBuf = newRunBuf();
		newBuf->length = length;
		memcpy( newBuf->data, data, length );
		inputStreamPrepend2( is, newBuf );

		return length;
	}
}

Tree *consumeTree( InputStream *is )
{
	while ( is->queue != 0 && is->queue->type == RunBufDataType && is->queue->offset == is->queue->length ) {
		RunBuf *runBuf = inputStreamPopHead2( is );
		free( runBuf );
	}

	if ( is->queue != 0 && (is->queue->type == RunBufTokenType || is->queue->type == RunBufIgnoreType) ) {
		RunBuf *runBuf = inputStreamPopHead2( is );

		/* FIXME: using runbufs here for this is a poor use of memory. */
		Tree *tree = runBuf->tree;
		free(runBuf);
		return tree;
	}

	return 0;
}

void undoConsumeTree( InputStream *is, Tree *tree, int ignore )
{
	/* Create a new buffer for the data. This is the easy implementation.
	 * Something better is needed here. It puts a max on the amount of
	 * data that can be pushed back to the inputStream. */
	RunBuf *newBuf = newRunBuf();
	newBuf->type = ignore ? RunBufIgnoreType : RunBufTokenType;
	newBuf->tree = tree;
	inputStreamPrepend2( is, newBuf );
}

struct LangEl *consumeLangEl( InputStream *is, long *bindId, char **data, long *length )
{
	if ( isSourceStream( is ) ) {
		Stream *stream = (Stream*)is->queue->tree;
		return stream->in->funcs->consumeLangEl( stream->in, bindId, data, length );
	}
	else {
		return 0;
	}
}

void undoConsumeLangEl( InputStream *is )
{
	if ( isSourceStream( is ) ) {
		Stream *stream = (Stream*)is->queue->tree;
		return stream->in->funcs->undoConsumeLangEl( stream->in );
	}
	else {
		assert(false);
	}
}


void prependData( InputStream *is, const char *data, long length )
{
	if ( isSourceStream( is ) ) {
		Stream *stream = (Stream*)is->queue->tree;
		return stream->in->funcs->prependData( stream->in, data, length );
	}
	else {
	//	#ifdef COLM_LOG_PARSE
	//	if ( colm_log_parse ) {
	//		cerr << "readying fake push" << endl;
	//	}
	//	#endif

	//	takeBackBuffered( inputStream );

		/* Create a new buffer for the data. This is the easy implementation.
		 * Something better is needed here. It puts a max on the amount of
		 * data that can be pushed back to the inputStream. */
		assert( length < FSM_BUFSIZE );

		RunBuf *newBuf = newRunBuf();
		newBuf->length = length;
		memcpy( newBuf->data, data, length );

		inputStreamPrepend2( is, newBuf );
	}
}

Tree *undoPrependData( InputStream *is, int length )
{
	if ( isSourceStream( is ) ) {
		Stream *stream = (Stream*)is->queue->tree;
		return stream->in->funcs->undoPrependData( stream->in, length );
	}
	else {
		if ( is->queue->type == RunBufDataType ) {
			char tmp[length];
			int have = 0;
			while ( have < length ) {
				int res = 0;
				getData( is, 0, tmp, length-have, &res );
				have += res;
			}
			return 0;
		}
		else {
			/* FIXME: leak here. */
			RunBuf *rb = inputStreamPopHead2( is );
			Tree *tree = rb->tree;
			free(rb);
			return tree;
		}
	}
}

void appendData( InputStream *is, const char *data, long len )
{
	while ( len > 0 ) {
		RunBuf *ad = newRunBuf();
		inputStreamAppend2( is, ad );

		long consume = 
			len <= (long)sizeof(ad->data) ? 
			len : (long)sizeof(ad->data);

		memcpy( ad->data, data, consume );
		ad->length = consume;

		len -= consume;
		data += consume;
	}
}

Tree *undoAppendData( InputStream *is, int length )
{
	if ( is->queueTail->type == RunBufDataType ) {
		char tmp[length];
		int have = 0;
		while ( have < length ) {
			int res = inputStreamDynamicGetDataRev2( is, tmp, length-have );
			have += res;
		}
		return 0;
	}
	else {
		/* FIXME: leak here. */
		RunBuf *rb = inputStreamPopTail2( is );
		Tree *tree = rb->tree;
		free(rb);
		return tree;
	}
}

void appendTree( InputStream *is, Tree *tree )
{
	RunBuf *ad = newRunBuf();

	inputStreamAppend2( is, ad );

	ad->type = RunBufTokenType;
	ad->tree = tree;
	ad->length = 0;
}

void appendStream( InputStream *in, struct ColmTree *tree )
{
	RunBuf *ad = newRunBuf();

	inputStreamAppend2( in, ad );

	ad->type = RunBufSourceType;
	ad->tree = tree;
	ad->length = 0;
}

Tree *undoAppendStream( InputStream *in )
{
	RunBuf *runBuf = inputStreamPopTail2( in );
	Tree *tree = runBuf->tree;
	free( runBuf );
	return tree;
}
