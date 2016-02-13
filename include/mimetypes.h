#ifndef HAVE_MIMETYPES_H
#define HAVE_MIMETYPES_H

typedef struct {
	char *type;
	char *exts;
} mimetype_t;

/**
 * TODO: Take filename as parameter
 * Returns number of types loaded, < 0 on failure.
 */
int mimetypes_load();

char *mimetypes_find(char *);

#endif
