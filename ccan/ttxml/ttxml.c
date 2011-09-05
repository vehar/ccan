/* Licensed under GPL - see LICENSE file for details */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ttxml.h"


#define BUFFER 3264


#define XML_LETTER	1
#define XML_NUMBER	2
#define XML_SPACE	4
#define XML_SLASH	8
#define XML_OPEN	16
#define XML_EQUALS	32
#define XML_CLOSE	64
#define XML_QUOTE	128
#define XML_OTHER	256

#define XML_ALL 0xFFFFFFFF


typedef struct XMLBUF
{
	FILE * fptr;
	char * buf;
	int len;
	int read_index;
	int eof;
} XMLBUF;


/* Allocate a new XmlNode */
XmlNode* xml_new(char * name)
{
	XmlNode * ret = malloc(sizeof(XmlNode));
	if(!ret)return NULL;

	ret->attrib = NULL;
	ret->nattrib = 0;
	ret->child = ret->next = NULL;

	ret->name = name;
	return ret;
}

/* free a previously allocated XmlNode */
void xml_free(XmlNode *target)
{
	int i;
	for(i=0; i<target->nattrib*2; i++)
		if(target->attrib[i])
			free(target->attrib[i]);

	if(target->attrib)free(target->attrib);
	if(target->child)xml_free(target->child);
	if(target->next)xml_free(target->next);
	free(target->name);
	free(target);
}

/* raise flags if we have a character of special meaning
 *
 * This is where I've hidden the switch statements :-p
 *
 */
int is_special(char item)
{
	if((item >= 'a' && item <= 'z') || (item >= 'A' && item <='Z'))
		return XML_LETTER;
	if( item >= '0' && item <='9' )
		return XML_NUMBER;
	if( item == 0x20 || item == '\t' ||	item == 0x0D || item == 0x0A )
		return XML_SPACE;
	if( item == '/' )
		return XML_SLASH;
	if( item == '<' )
		return XML_OPEN;
	if( item == '=' )
		return XML_EQUALS;
	if( item == '>' )
		return XML_CLOSE;
	if( item == '"' || item == '\'' )
		return XML_QUOTE;
	return 128;
}

/* Refresh the buffer, expects not to be called when EOF */
static void xml_read_file(XMLBUF *xml)
{
	int size;
	
	size = fread( xml->buf,	1, xml->len, xml->fptr);
	if( size != xml->len )
	{
		xml->len = size;
		xml->buf[size]=0;
		xml->eof = 1;
	}
}


/* All reading of the XML buffer done through these two functions */
/*** read a byte without advancing the offset */
static char xml_peek(XMLBUF *xml)
{
	return xml->buf[xml->read_index];
}

/*** read a byte and advance the offset */
static char xml_read_byte(XMLBUF *xml)
{
	char ret = xml_peek(xml);
	xml->read_index++;
	if(xml->read_index >= xml->len)
	{
		if(xml->eof)return ret;
		xml->read_index = 0 ;
		xml_read_file(xml);
	}
	return ret;
}


/* skip over bytes matching the is_special mask */
static void xml_skip( XMLBUF *xml, int mask)
{
	printf("just called\n");
	while( is_special(xml_peek(xml)) & mask && xml->len )
		xml_read_byte(xml);
}


/* character matching tests for the feed functions */
static char quotechar = 0;
static int test_quote(const char x)
{
	static int escaped=0;
	if( escaped || '\\' == x )
	{
		escaped = !escaped;
		return 1;
	}
	if( x != quotechar )
		return 1;
	return 0;
}

static int feed_mask = 0;
static int test_mask(const char x)
{
	return !(is_special(x) & feed_mask);
}

/*
 * char* xml_feed(x, test)
 *
 * Reads as many contiguous chars that pass test() into a newly allocated
 * string.
 *
 * Instead of calling xml_read_byte and flogging realloc() for each byte,
 * it checks the buffer itself.
*/
static char* xml_feed( XMLBUF *xml, int (*test)(char) )
{
	int offset = xml->read_index;
	int delta;
	char *ret = NULL;
	int size = 0;

	/* perform first and N middle realloc()'s */
	while( test(xml->buf[offset]) )
	{
		offset ++;

		if(offset >= xml->len)
		{
			delta = offset - xml->read_index;
			ret = realloc(ret, size + delta + 1);
			memcpy(ret+size, xml->buf + xml->read_index, delta);
			size += delta;
			ret[size]=0;
			if(xml->eof)return ret;
			xml_read_file(xml);
			xml->read_index = 0;
			offset = 0;
		}
	}
	/* perform final realloc() if needed */
	if(offset > xml->read_index)
	{
		delta = offset - xml->read_index;
		ret = realloc(ret, size + delta + 1);
		memcpy(ret+size, xml->buf + xml->read_index, delta);
		xml->read_index = offset;
		size += delta;
		ret[size]=0;
	}
	return ret;
}

/* this reads attributes from tags, of the form...
 *
 * <tag attr1="some arguments" attr2=argument>
 *
 * It is aware of quotes, and will allow anything inside quoted arguments
 */
static void xml_read_attr(struct XMLBUF *xml, XmlNode *node)
{
	int n=0;

	// how does this tag finish?
	while(xml->len)
	{
		if( is_special(xml_peek(xml)) & (XML_CLOSE | XML_SLASH) )
			return;

		n = ++node->nattrib;
		node->attrib = realloc(node->attrib, n * 2 * sizeof(char*) );
		node->attrib[--n*2+1] = 0;
		
		feed_mask = XML_EQUALS | XML_SPACE | XML_CLOSE | XML_SLASH;
		node->attrib[n*2] = xml_feed(xml, test_mask );
		if( xml_peek(xml) == '=' )
		{
			xml_read_byte(xml);
			if( is_special(xml_peek(xml)) & XML_QUOTE )
			{
				quotechar = xml_read_byte(xml);
				node->attrib[n*2+1] = xml_feed(xml, test_quote);
				xml_read_byte(xml);
			}
			else
			{
				feed_mask = XML_SPACE | XML_CLOSE | XML_SLASH;
				node->attrib[n*2+1] = xml_feed(xml, test_mask);
			}
		}
		xml_skip(xml, XML_SPACE);
	}
}

/* The big decision maker, is it a regular node, or a text node.
 * If it's a node, it will check if it should have children, and if so
 * will recurse over them.
 * Text nodes don't have children, so no recursing.
 */
static XmlNode* xml_parse(struct XMLBUF *xml)
{
	int offset;
	int toff;
	char *tmp;
	XmlNode **this, *ret = NULL;
	
	this = &ret;

	xml_skip(xml, XML_SPACE);	// skip whitespace
	offset=0;
	while( (xml->read_index < xml->len) || !xml->eof )
	{
		switch(is_special(xml_peek(xml)))
		{
			case XML_OPEN:
				xml_read_byte(xml);
				if(xml_peek(xml) == '/')
					return ret;		// parents close tag
				// read the tag name
				feed_mask = XML_SPACE | XML_SLASH | XML_CLOSE;
				*this = xml_new( xml_feed(xml, test_mask));
				xml_skip(xml, XML_SPACE);	// skip any whitespace

				xml_read_attr(xml, *this);	// read attributes

				// how does this tag finish?
				switch(is_special(xml_peek(xml)))
				{
					case XML_CLOSE:		// child-nodes ahead
						xml_read_byte(xml);
						(*this)->child = xml_parse(xml);
						xml_skip(xml, XML_ALL ^ XML_CLOSE);
						xml_read_byte(xml);
						break;
					case XML_SLASH:		// self closing tag
						xml_read_byte(xml);
						xml_read_byte(xml);
						break;
				}
				break;

			default:	// text node
				*this = xml_new(0);
				xml_skip(xml, XML_SPACE);	// skip any whitespace
				feed_mask = XML_OPEN;
				(*this)->nattrib=1;
				(*this)->attrib = malloc(sizeof(char*)*2);
				(*this)->attrib[1] = NULL;
				tmp = (*this)->attrib[0] = xml_feed(xml, test_mask);

				/* trim the whitespace off the end of text nodes,
				 * by overwriting the spaces will null termination. */
				toff = strlen(tmp)-1;
				while( ( is_special(tmp[toff]) & XML_SPACE ) )
				{
					tmp[toff] = 0;
					toff --;
				}

				break;
		}
		this = &(*this)->next; 
		xml_skip(xml, XML_SPACE);	// skip whitespace
	}	

	return ret;
}


/* bootstrap the structures for xml_parse() to be able to get started */
XmlNode* xml_load(const char * filename)
{
	struct XMLBUF xml;
	XmlNode *ret = NULL;

//	printf("xml_load(\"%s\");\n", filename);

	xml.eof = 0;
	xml.read_index = 0;
	xml.fptr = fopen(filename, "rb");
	if(!xml.fptr)
	{
		printf("Opening file failed\n");
		return NULL;
	}

	xml.buf = malloc(BUFFER+1);
	xml.buf[BUFFER]=0;
	if(!xml.buf)
		goto xml_load_fail_malloc_buf;
	
	xml.len = fread(xml.buf, 1, BUFFER, xml.fptr);
	if(xml.len < BUFFER)
		xml.eof = 1;

	ret = xml_parse(&xml);

	free(xml.buf);
xml_load_fail_malloc_buf:
	fclose(xml.fptr);
	return ret;
}

/* very basic function that will get you the first node with a given name */
XmlNode * xml_find(XmlNode *xml, const char *name)
{
	XmlNode * ret;
	if(xml->name)if(!strcmp(xml->name, name))return xml;
	if(xml->child)
	{
		ret = xml_find(xml->child, name);
		if(ret)return ret;
	}
	if(xml->next)
	{
		ret = xml_find(xml->next, name);
		if(ret)return ret;
	}
	return NULL;
}

/* very basic attribute lookup function */
char* xml_attr(XmlNode *x, const char *name)
{
	int i;
	for(i=0; i<x->nattrib; i++)
		if(x->attrib[i*2])
			if(!strcmp(x->attrib[i*2], name))
				return x->attrib[i*2+1];
	return 0;
}


#ifdef TEST
/* print out the heirarchy of an XML file, useful for debugging */
void xp(XmlNode *x, int level, int max)
{
	int i;
	char text[] = "text";
	char *name = text;
	if(level > max)return;
	if(!x)return;
	if(x->name)name = x->name;
	for(i=0; i<level; i++)printf("    ");
	printf("%s:", name);
	if(x->name)
	for(i=0; i<x->nattrib; i++)
		printf("%s=\"%s\",", x->attrib[i*2], x->attrib[i*2+1]);
	else printf("%s", x->attrib[0]);
	printf("\n");
	if(x->child)xp(x->child, level+1, max);
	if(x->next)xp(x->next, level, max);
}


int main(int argc, char *argv[])
{
	XmlNode *x, *tmp;
	int i;

	if(!argv[1])
	{
		printf("USAGE: %s name\n\t reads name where name is an XML file.\n",
				argv[0]);
		return 1;
	}

#ifdef PROFILE
	for(i=0; i<1000; i++)
	{
#endif
		x = xml_load(argv[1]);

		if(!x)
		{
			printf("Failed to load.\n");
			return 2;
		}
#ifndef PROFILE
		xp(x, 1, 20);
#endif
		xml_free(x);
#ifdef PROFILE
	}
#endif

	
//	tmp = xml_find(x, "geometry");
//	xp(x, 1, 6);
//	printf("Happily free.\n");
	return 0;
}
#endif

