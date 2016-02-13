#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "mimetypes.h"

mimetype_t **_mimetypes_list = NULL;
int _mimetypes_num = 0;

int mimetypes_load() {
	FILE *fil = NULL;
	int sz = 4096;
	char *line = malloc(sz), *ptr, *exts;
	mimetype_t *mime = NULL;
	mimetype_t **newarr = NULL;

	if( line == NULL )
		return -1;

	fil = fopen("/etc/mime.types","r");
	if( fil == NULL ) {
		free(line);
		return -1;
	}

	memset(line,0,sz);
	while( fgets(line,sz,fil) ) {
		// SKip comments, blank or empty lines
		if( *line == '#' || *line == 0 || *line == '\r' || *line == '\n' )
			continue;

		// Find first tab or space
		ptr = strchr(line,'\t');
		if( ptr == NULL )
			ptr = strchr(line,' ');

		// No tabe or space, probably no extensions
		if( ptr == NULL )
			continue;

		*ptr++ = 0;

		// White spaces or tabs, mindful of end of string
		while( *ptr != 0 && (*ptr == '\t' || *ptr == ' ') ) {
			// Newline bad
			if( *ptr == '\r' || *ptr == '\n' )
				break;

			ptr++;
		}

		// Bunch of spaces = bad
		if( *ptr == '\r' || *ptr == '\n' || *ptr == 0 )
			continue;

		// Out of laziness we'll use strstr(' '+ext+' ')
		ptr--;
		*ptr = ' ';
		exts = ptr;

		// Either newline or end of string
		while( *ptr != '\r' && *ptr != '\n' && *ptr != 0 )
			ptr++;

		// This is bad.
		if( ptr == exts )
			break;
		*ptr = 0;

		// Mimetype info struct
		mime = malloc(sizeof(mimetype_t));
		if( mime == NULL )
			continue;

		// Set type
		mime->type = strdup(line);
		if( mime->type == NULL ) {
			free(mime);
			continue;
		}

		// Set extensions
		mime->exts = malloc(strlen(exts)+2);
		if( mime->exts == NULL ) {
			free(mime->type);
			free(mime);
			continue;
		}
		sprintf(mime->exts,"%s ",exts);

		if( _mimetypes_list == NULL ) {
			_mimetypes_num = 0;
			newarr = malloc( sizeof(mimetype_t) );
		} else {
			newarr = realloc( _mimetypes_list, sizeof(mimetype_t) * _mimetypes_num );
		}

		if( newarr == NULL ) {
			free(mime->type);
			free(mime->exts);
			free(mime);
			continue;
		}

		newarr[_mimetypes_num++] = mime;
		_mimetypes_list = newarr;
	}

	free(line);
	fclose(fil);
	return _mimetypes_num;
}

char *mimetypes_find(char *fil) {
	char *ret = NULL, *pos, *dest;
	int x;

	pos = strrchr(fil,'.');
	if( pos != NULL ) {
		pos++;

		dest = malloc(strlen(pos)+3);
		if( dest != NULL ) {
			sprintf(dest," %s ",pos);
			for( x=0;x<_mimetypes_num;x++ ) {
				if( strstr(_mimetypes_list[x]->exts,dest) != NULL ) {
					ret = _mimetypes_list[x]->type;
					break;
				}
			}
			free(dest);
		}
	}

	return (ret==NULL)?"application/octet-stream":ret;
}
