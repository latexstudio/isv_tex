/*
  Generate a PDF of the ISV bible (or another translation).
  Routines to parse the LaTeX formatted version of the ISV books to
  generate the text units that need to be rendered.

  Note that the text of the ISV is copyright, and is not part of this
  program, even it comes bundled together, and thus is not touched by
  the GPL.  

  This program is copyright Paul Gardner-Stephen 2014, and is offered
  on the following basis:
  
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <ctype.h>
#include "hpdf.h"
#include "generate.h"

/* Basically we want to build a tree structure where we parse out the
   different LaTeX tags and attach the text present in each tag in the
   appropriate branch of the tree.  The text itself should be parsed into
   "words", i.e., things that are separated from one another by space.

   Complications are introduced by the need to keep track of verses, and
   their accumulated foot notes, so that pages can be filled, taking into
   account the space required for the footnotes.  Because the page fill
   decision process happens late, for now we will just accumulate the
   footnotes into a list.
   
*/

int token_count=0;
int token_types[MAX_TOKENS];
char *token_strings[MAX_TOKENS];

int clear_tokens()
{
  int i;
  for(i=0;i<token_count;i++) {
    if (token_strings[i]) { free(token_strings[i]); token_strings[i]=NULL; }
  }
  token_count=0;
  return 0;
}

int next_file_token(struct parsed_text *p,
		    int token_type,int token_len,char *token_text)
{
  token_text[token_len]=0;
  if (0) printf("Token: type=%d, len=%d,text=%s\n",token_type,token_len,
		token_text);

  if (token_count>=MAX_TOKENS) {
    fprintf(stderr,"Too many tokens in tex file/tex file too large.\n");
    exit(-1);
  }
  
  token_types[token_count]=token_type;
  if (token_len) token_strings[token_count]=strdup(token_text);
  else token_strings[token_count]=NULL;
  token_count++;
  
  return 0;
}

int tokenise_file(char *filename, int crossreference_parsing)
{
  FILE *f=fopen(filename,"r");
  if (!f) {
    fprintf(stderr,"Could not read latex file '%s'\n",filename);
    exit(-1);
  }
  fseek(f,0,SEEK_END);
  off_t fileLength=ftello(f);
  
  struct parsed_text *p=calloc(sizeof(struct parsed_text),1);
  p->type=PT_ROOT;

  // Memory map the file for efficient processing
  unsigned char *file=mmap(NULL, fileLength, PROT_READ, MAP_SHARED,
			   fileno(f), 0);
  if (file==MAP_FAILED) {
    fprintf(stderr,"Could not memory map file '%s'\n",filename);
    exit(-1);    
  }

  // File is memory mapped, so we can tokenise
  int i;
#define PS_NORMAL 0
#define PS_SLASH 1
#define PS_COMMENT 2
  int parse_state=PS_NORMAL;

  int token_len=0;
  char token_text[1024];
  int token_type=0;

  int line_num=1;
  
  for(i=0;i<fileLength;i++) {
    if (file[i]=='\n'||file[i]=='\r') line_num++;
    switch(parse_state) {
    case PS_COMMENT:
      switch(file[i]) {
      case '\r': case '\n':
	parse_state=PS_NORMAL;
      }
      break;
    case PS_NORMAL:
      switch(file[i]) {
      case '\\':
	parse_state=PS_SLASH; break;
      case ' ':
	// - don't break "book chap:verse" into separate tokens when parsing
	//   the cross-reference database
	if (token_len&&(crossreference_parsing)&&(isalpha(token_text[token_len-1])))
	  {
	    token_text[token_len]=0;
	    if (token_len<1023) token_text[token_len++]=file[i];
	    else {
	      include_show_stack();
	      fprintf(stderr,"%s:%d:Token or line too long.\n",
		      file,line_num);
	      exit(-1);
	    }
	    break;
	  }
	// FALL THROUGH
      case '\r': case '\n': case '\t':
	// white space, so end token
	if (
	    // - skip any number of space and tab as a single token.
	    token_len) {
	  // Got a token, so pass it up, and reset token status
	  next_file_token(p,token_type,token_len,token_text);
	  token_len=0;
	  token_type=TT_TEXT;
	}

	// merge any number of spaces together
	while (file[i]==' '&&file[i+1]==' ') i++;
	
	// Is this an end of paragraph?
	if ((file[i]=='\r'||file[i]=='\n')
	    &&(file[i+1]=='\r'||file[i+1]=='\n'))
	  {
	    while (file[i+1]=='\r'||file[i+1]=='\n') i++;
	    next_file_token(p,TT_PARAGRAPH,0,token_text);
	  } else {
	  // If not new paragraph, then it indicates some white space
	  next_file_token(p,TT_SPACE,0,token_text);
	}

	break;
      case '{':
	// start of tag token: output accumulated text, and then
	// an empty-named tag
	token_text[token_len]=0;
	if (token_len) {
	  next_file_token(p,token_type,token_len,token_text);
	}
	if (token_type!=TT_TAG) next_file_token(p,TT_TAG,0,token_text);
	token_len=0;
	token_type=TT_TEXT;
	token_text[0]=0;
	break;
      case '}':
	// end of tag token
	if (token_len) {
	  // Got a token, so pass it up, and reset token status
	  token_text[token_len]=0;
	  next_file_token(p,token_type,token_len,token_text);
	  token_len=0;
	  token_type=TT_TEXT;
	}
	// and also report the end of tag
	next_file_token(p,TT_ENDTAG,0,token_text);
	break;
      default:
	token_text[token_len]=0;

	// Put a space between text and em-dash
	if (fileLength-i>2)
	  if ((file[i]=='-')&&(file[i+1]=='-')&&(file[i+2]=='-')) {
	    if ((token_len&&(token_text[token_len-1]!='-'))
		||((!token_len)&&(token_types[token_count-1]!=TT_SPACE))) {
	      // A non-breaking space would be ideal to prevent em-dashes appearing
	      // at the start of a line.  However, they do still need to be elastic.
	      if (token_len) next_file_token(p,token_type,token_len,token_text);
	      token_text[0]=0; token_len=0; token_type=TT_TEXT;
	      next_file_token(p,TT_NONBREAKINGSPACE,0,token_text);
	      fprintf(stderr,"Inserting non-breaking space before em-dash\n");
	    }
	  }
	if (token_type==TT_TAG&&(!strcmp(token_text,"allowbrea"))&&(file[i]=='k'))
	  {
	    // \allowbreak - implemented by emitting nothing -- the presence
	    // of the tag has introduced the break.
	    token_type=TT_TEXT;
	    token_len=0;
	  }
	else if ((file[i]==',')
		 &&(file[i+1]!='"')
		 &&(file[i+1]!=',')
		 &&(file[i+1]!='.')
		 &&(file[i+1]!='\'')
		 )
	  {
	  // Break text after certain forms of punctuation
	  // (unless other right-hangable punctuation follows)
	  token_text[token_len++]=',';
	  token_text[token_len]=0;
	  next_file_token(p,token_type,token_len,token_text);
	  token_type=TT_TEXT;
	  token_len=0;
	} else {
	  if (file[i]=='%'&&(token_len==0)) {
	    // Start of a comment
	    parse_state=PS_COMMENT;
	  }
	  else if (token_len<1023) {
	    token_text[token_len++]=file[i];

	    unicodify(token_text,&token_len,1023,
		      (i<fileLength?file[i+1]:0x00));
	    if ((i>1)&&(file[i-2]=='-')&&(file[i-1]=='-')&&(file[i]=='-'))
	      {
		// Em-dash.  End token here and insert a space token as well
		// (unless a space already follows)
		token_text[token_len]=0;
		next_file_token(p,token_type,token_len,token_text);
		token_text[0]=0; token_len=0; token_type=TT_TEXT;
		if ((file[i+1]!=' ')&&(file[i+1]!='\r')&&(file[i+1]!='\n'))
		  next_file_token(p,TT_SPACE,0,token_text);
	      }
	  } else {
	    include_show_stack();
	    fprintf(stderr,"%s:%d:Token or line too long.\n",
		    file,line_num);
	    exit(-1);
	  }
	}
	break;
      }
      break;
    case PS_SLASH:
      switch(file[i]) {
	// Check for latex escape characters (literals)
      case '@': case '&': case '%':
	parse_state = PS_NORMAL;
	token_text[token_len++]=file[i];
	break;
	// Check for latex force line break
      case '\\': 
	parse_state = PS_NORMAL;
	token_text[token_len++]='\r';
	break;
	// Thin space
      case ',':
#ifdef NO_UNICODE
	parse_state = PS_NORMAL;
	token_text[token_len]=0;
	if (token_len)
	  next_file_token(p,token_type,token_len,token_text);

	token_text[0]=0;
	next_file_token(p,TT_THINSPACE,0,token_text);
	token_type= TT_TEXT;
	token_len=0;
#else
	// Add non-breaking thin-space unicode point (0x202f)
	// Well, libharu doesn't support it, so we have to settle for a
	// regular non-breaking space (0xa0)
	token_text[token_len++]=0xC2;
	token_text[token_len++]=0xA0;
	token_text[token_len]=0;
#endif
	break;
      default:
	// not an escape character, so assume it is a label
	if (token_len!=0) {
	  // slash terminates an existing token, so process that first.
	  next_file_token(p,token_type,token_len,token_text);
	  parse_state = PS_NORMAL;
	  token_type=TT_TAG;
	  token_len=0;
	  token_text[token_len++]=file[i];
	} else {
	  // slash is at start of a token
	  parse_state = PS_NORMAL;
	  token_type=TT_TAG;
	  token_len=0;
	  token_text[token_len++]=file[i];
	}
	break;
      }
      break;
    default:
      parse_state=PS_NORMAL;
      break;
    } 
  }
  
  munmap(file,fileLength);
  fclose(f);

  return 0;
  }
