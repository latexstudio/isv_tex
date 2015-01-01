/*
  Generate a PDF of the ISV bible (or another translation).

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
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include "ft2build.h"
#include FT_FREETYPE_H
#include "hpdf.h"
#include "generate.h"

void log_free(void *p,const char *file,int line,const char *function)
{
  fprintf(stderr,"%s:%d: free(%p) called from %s()\n",
	  file,line,p,function);
  free(p);
  return;
}

// Uncomment to help debug double-frees
// #define free(X) log_free(X,__FILE__,__LINE__,__FUNCTION__)

int line_clone_piece(struct piece *p,struct piece *clone)
{
  bcopy(p,clone,sizeof(struct piece));
  clone->piece=strdup(p->piece);
  // fprintf(stderr,"Cloned %p (\"%s\") to be %p\n",p->piece,p->piece,clone->piece);
  return 0;
}

/* Clone a line */
struct line_pieces *line_clone(struct line_pieces *l)
{
  struct line_pieces *clone=calloc(sizeof(struct line_pieces),1);

  // Copy contents of original line to clone
  bcopy(l,clone,sizeof(struct line_pieces));

  // strdup all strings
  int i;
  for(i=0;i<clone->piece_count;i++) line_clone_piece(&l->pieces[i],&clone->pieces[i]);

  return clone;
}

int line_calculate_height(struct line_pieces *l)
{
  int max=-1; int min=0;
  int linegap=0;
  int i;
  if (0)
    fprintf(stderr,"Calculating height of line %p (%d pieces, %.1fpts wide, align=%d, left margin=%d)\n",
	    l,l->piece_count,l->line_width_so_far,l->alignment,l->left_margin);

  // insert_vspace() works by making a line with zero pieces, and the space to skip
  // is set in l->line_height, so we don't need to do anything in that case.
  if (l->piece_count==0) {
    l->ascent=l->line_height;
    l->descent=0;
    // fprintf(stderr,"  Line is vspace(%.1fpt)\n",l->line_height);
    return 0;
  }
  
  for(i=0;i<l->piece_count;i++)
    {
      // Get ascender height of font
      int ascender_height
	=HPDF_Font_GetAscent(l->pieces[i].font->font)
	*l->pieces[i].font->font_size/1000;
      // Get descender depth of font
      int descender_depth
	=HPDF_Font_GetDescent(l->pieces[i].font->font)
	*l->pieces[i].font->font_size/1000;
      if (0) fprintf(stderr,"  '%s' is %.1fpt wide.\n",
		     l->pieces[i].piece,l->pieces[i].piece_width);
      if (descender_depth<0) descender_depth=-descender_depth;
      // Don't count the space used by dropchars, since it gets covered by
      // the extra line(s) of the dropchar.
      if (l->pieces[i].font->line_count==1) {
	if (ascender_height-l->pieces[i].piece_baseline>max)
	  max=ascender_height-l->pieces[i].piece_baseline;
	if (l->pieces[i].piece_baseline-descender_depth<min)
	  min=l->pieces[i].piece_baseline-descender_depth;
      }
      if (l->pieces[i].font->line_count==1) {
	if (l->pieces[i].font->linegap>linegap)
	  linegap=l->pieces[i].font->linegap;
    }

    }

  // l->line_height=max-min+1;
  l->line_height=linegap*line_spacing;
  l->ascent=max; l->descent=-min;
  // fprintf(stderr,"  line ascends %dpts and descends %d points.\n",max,-min);
  return 0;
}

// Setup a line

int line_apply_poetry_margin(struct paragraph *p,struct line_pieces *current_line)
{
  // If we are in poetry mode, then apply margin.
  if (p->poem_level) {
    current_line->left_margin
      =poetry_left_margin
      +(p->poem_level-1)*poetry_level_indent
      +p->poem_subsequent_line*poetry_wrap_indent;
    current_line->max_line_width
      =page_width-left_margin-right_margin-current_line->left_margin;
    if (0)
      fprintf(stderr,"Applying indent of %dpts in poetry mode (level=%d, subs=%d).\n",
	      current_line->left_margin,p->poem_level,p->poem_subsequent_line);
    p->poem_subsequent_line=1;
  }
  return 0;
}

int line_free(struct line_pieces *l)
{
  int i;
  if (!l) return 0;
  if (l->freed) {
    fprintf(stderr,"Being asked to free a line that has already been freed.\n");
    line_dump(l);
    exit(-1);
  }
  if (0) {
    fprintf(stderr,"Freeing line with uid = %d\n",l->line_uid);
    line_dump(l);
  }
  for(i=0;i<l->piece_count;i++)
    if (l->pieces[i].piece) {
      if (0) fprintf(stderr,"  freeing piece #%d (%p) (\"%s\")\n",
		     i,l->pieces[i].piece,l->pieces[i].piece);
      free(l->pieces[i].piece); l->pieces[i].piece=NULL; }
  l->freed=1;
  free(l);
  return 0;
}

float calc_left_hang(struct line_pieces *l,int left_hang_piece)
{
  if (left_hang_piece>=l->piece_count) return 0.0;
  
  char *text=l->pieces[left_hang_piece].piece;
  char hang_text[1024];

  int o=0;
  hang_text[0]=0;
  while(o<1024&&text[o]) {
    int codepoint=0;
    int bytes=1;
    if (text[o]&0x80) {
      // Unicode
      if (!(text[o]&0x20)) {
	// 2 byte code point
	codepoint
	  =((text[o]&0x1f)<<6)
	  |((text[o+1]&0x3f)<<0);
	bytes=2;
      } else if (!(text[o]&0x10)) {
	// 3 byte code point
	codepoint
	  =((text[o]&0x0f)<<12)
	  |((text[o+1]&0x3f)<<6)
	  |((text[o+2]&0x3f)<<0);
	bytes=3;
      } else {
	// 4 byte code point
	codepoint
	  =((text[o]&0x07)<<18)
	  |((text[o+1]&0x3f)<<12)
	  |((text[o+2]&0x3f)<<6)
	  |((text[o+3]&0x3f)<<0);
	bytes=4;
      }
    } else {
      codepoint=text[o];
      bytes=1;
    }

    if (0)
      fprintf(stderr,"    considering codepoint 0x%04x for hanging (%d bytes, o=%d)\n",
	    codepoint,bytes,o);
    if (unicodePointIsHangable(codepoint)) {
      for(int i=0;i<bytes;i++) hang_text[o+i]=text[o+i];
      o+=bytes;
      hang_text[o]=0;
      if (0) fprintf(stderr,"      hang_text='%s'\n",hang_text);
      continue;
    }
    break;
  }
  
  if (hang_text[0]) {
    set_font(l->pieces[left_hang_piece].font->font_nickname);
    float hang_width=HPDF_Page_TextWidth(page,hang_text);
    if (0) fprintf(stderr,"Hanging '%s' on the left (%.1f points)\n",
		   hang_text,hang_width);
    return hang_width;
  } else return 0.0;
}

int line_recalculate_width(struct line_pieces *l)
{
  // Recalculate line width
  int i;

  // Work out basic width.
  // At the same time, look for footnotes that can be placed above punctuation
  int footnotemark_index=set_font("footnotemark");
  l->line_width_so_far=0;
  for(i=0;i<l->piece_count;i++) {
    l->pieces[i].piece_width=l->pieces[i].natural_width;

    if (0) fprintf(stderr,"  piece '%s' is %.1fpts wide.\n",
		   l->pieces[i].piece,
		   l->pieces[i].natural_width);
    
    if (i  // make sure there is a previous piece
	&&l->pieces[i].font==&type_faces[footnotemark_index]) {
      // This is a footnote mark.
      // Check if the preceeding piece ends in any low punctuation marks.
      // If so, then discount the width of that piece by the width of such
      // punctuation. If the width of the discount is wider than this footnotemark,
      // then increase the width of this footnotemark so that the end of text
      // position is advanced correctly.
      if (0) {
	fprintf(stderr,"hanging footnotemark over punctuation: ");
	line_dump(l);
      }
      char *text=l->pieces[i-1].piece;
      int o=strlen(text);
      char *hang_text=NULL;
      while(o>0) {
	switch(text[o-1]) {	    
	case '.': case ',':
	case '-': case ' ': 
	  hang_text=&text[--o];
	  continue;
	}
	break;
      }
      set_font(l->pieces[i-1].font->font_nickname);
      float hang_width=HPDF_Page_TextWidth(page,hang_text);
      float all_width=l->pieces[i-1].natural_width;
      if (0) fprintf(stderr,"  hang_width=%.1f, hang_text='%s', all_width=%.1f\n",
		     hang_width,hang_text?hang_text:"",all_width);
      l->pieces[i-1].piece_width=all_width-hang_width;
      if (hang_width>l->pieces[i].piece_width) l->pieces[i].piece_width=hang_width;
      if (0) {
	fprintf(stderr,"  This is the punctuation over which we are hanging the footnotemark: [%s] (%.1fpts)\n",hang_text,hang_width);
	fprintf(stderr,"Line after hanging footnote over punctuation: ");
	line_dump(l);
      }
    }

    // Related to the above, we must discount the width of a dropchar if it is
    // followed by left-hangable material
    if ((i==1)&&(l->pieces[0].font->line_count>1))
      {
	int piece=i;
	float discount=0;
	
	// Discount any footnote
	if (l->pieces[piece].font==&type_faces[footnotemark_index]) {
	  discount+=l->pieces[i].natural_width;
	  piece++;
	}
	discount+=calc_left_hang(l,piece);       
	
	l->pieces[0].piece_width=l->pieces[0].natural_width-discount;
      }
  }

  for(i=0;i<l->piece_count;i++) l->line_width_so_far+=l->pieces[i].piece_width;

  l->left_hang=0;
  l->right_hang=0;

  // Now discount for hanging verse numbers, footnotes and punctuation.
  int left_hang_piece=0;
  if (l->piece_count) {
    if (!(strcmp(l->pieces[0].font->font_nickname,"versenum"))) {
      // Verse number on the left.
      // Only hang single-digit or skinny (10-19) verse numbers
      // Actually, let's just hang all verse numbers. I think it looks better.
      int vn=atoi(l->pieces[0].piece);      
      if (vn<999) {
	l->left_hang=l->pieces[0].piece_width;	
	left_hang_piece=1;
	if (0) fprintf(stderr,
		       "Hanging verse number '%s'(=%d) in left margin (%.1f points)\n",
		       l->pieces[0].piece,vn,l->left_hang);
      }
    }

    char *text=NULL;
    char *hang_text=NULL;

    // Check for hanging punctuation (including if it immediately follows a
    // verse number)
    if (left_hang_piece<l->piece_count) {
      l->left_hang+=calc_left_hang(l,left_hang_piece);
    }

    // Now check for right hanging
    hang_text=NULL;
    int right_hang_piece=l->piece_count-1;

    // Ignore white-space at the end of lines when working out hanging.
    while ((right_hang_piece>=0)
	   &&l->pieces[right_hang_piece].piece
	   &&strlen(l->pieces[right_hang_piece].piece)
	   &&(l->pieces[right_hang_piece].piece[0]==' '))
      right_hang_piece--;

    float hang_note_width=0;
    float hang_width=0;
    
    if (right_hang_piece>=0) {
      // Footnotes always hang 
      if (!(strcmp(l->pieces[right_hang_piece].font->font_nickname,"footnotemark"))) {
	hang_note_width=l->pieces[right_hang_piece].natural_width;
	l->right_hang=l->pieces[right_hang_piece--].piece_width;
      }
    }

    if (right_hang_piece>=0&&(right_hang_piece<l->piece_count)) {
      text=l->pieces[right_hang_piece].piece;
      int textlen=strlen(text);

      if (text) {
	// Now look for right hanging punctuation
	int o=textlen-1;
	while(o>=0) {
	  if (0) fprintf(stderr,"Requesting prev code point from \"%s\"[%d]\n",
			 text,o);
	  int codepoint=unicodePrevCodePoint(text,&o);
	  if (codepoint&&unicodePointIsHangable(codepoint)) {
	    if (0) fprintf(stderr,"Decided [%s] is hangable (o=%d)\n",
			   unicodeToUTF8(codepoint),o);
	    hang_text=&text[o+1];
	  } else
	    break;
	}
	
	if (hang_text) {
	  set_font(l->pieces[right_hang_piece].font->font_nickname);
	  hang_width=HPDF_Page_TextWidth(page,hang_text);
	  // Reduce hang width by the amount of any footnote hang over
	  // the punctuation.
	  hang_width-=(l->pieces[right_hang_piece].natural_width
		       -l->pieces[right_hang_piece].piece_width);
	  // Only hang if it won't run into things on the side.
	  // XX Narrowest space is probably between body and
	  // cross-refs, so use that measure regardless of whether
	  // we are on a left or right face.
	  int max_hang_space
	    =right_margin
	    -crossref_margin_width-crossref_column_width
	    -2;  // plus a little space to ensure some white space
	  if (hang_width+hang_note_width<=max_hang_space) {
	    l->right_hang=hang_note_width+hang_width;
	    if (0) {
	      fprintf(stderr,"Hanging '%s' in right margin (%.1f points, font=%s)\n",
		      hang_text,hang_width,
		      l->pieces[right_hang_piece].font->font_nickname);
	    }
	  } else l->right_hang=hang_note_width;
	}
      }
    }
  }
  l->line_width_so_far-=l->left_hang+l->right_hang;
  return 0;
}

int line_emit(struct paragraph *p,int line_num,int isBodyParagraph,
	      int drawingPage)
{
  
  struct line_pieces *l=p->paragraph_lines[line_num];
  int break_page=0;

  // fprintf(stderr,"Emitting line: "); line_dump(p->paragraph_lines[line_num]);
  
  // Work out maximum line number that we have to take into account for
  // page fitting, i.e., to prevent orphaned heading lines.
  int max_line_num=line_num;
  float combined_line_height=l->line_height;
  // fprintf(stderr,"  line itself (#%d) is %.1fpts high\n",line_num,l->line_height);
  while ((max_line_num<(p->line_count-1))
	 &&p->paragraph_lines[max_line_num]->tied_to_next_line) {
    combined_line_height+=p->paragraph_lines[++max_line_num]->line_height;
    if (0) {
      fprintf(stderr,"  dependent line is %.1fpts high:",
	      p->paragraph_lines[max_line_num]->line_height);
      line_dump(p->paragraph_lines[max_line_num]);
    }
  }
  if (0)
    fprintf(stderr,"Treating lines %d -- %d as a unit %.1fpts high\n",
	  line_num,max_line_num,combined_line_height);
  
  // Does the line(s) require more space than there is?    
  float baseline_y=page_y+combined_line_height*line_spacing;
  if (baseline_y>(page_height-bottom_margin)) {
    if (0)
      fprintf(stderr,"Breaking page %d at %.1fpts to make room for body text\n",
	      current_page,page_y);
    break_page=1;
    page_penalty_add((baseline_y-bottom_margin)*OVERFULL_PAGE_PENALTY_PER_PT);
  }

  // Does the line plus footnotes require more space than there is?
  // - clone footnote paragraph and then append footnotes referenced in this
  // line to the clone, then measure its height.
  // - deduct footnote space from remaining space.
  int footnotes_total_height=0;
  if (isBodyParagraph) {
    struct paragraph temp;
    paragraph_init(&temp);
    paragraph_clone(&temp,&footnote_paragraph);
    current_line_flush(&temp);
    struct paragraph *f=layout_paragraph(&temp);
    
    int footnotes_height=paragraph_height(f);
    baseline_y+=footnotes_height;
    baseline_y+=footnote_sep_vspace;
    footnotes_total_height=footnotes_height+footnote_sep_vspace;
    if (0) {
      fprintf(stderr,"Unrendered footnote block is:\n");
      paragraph_dump(&footnote_paragraph);
      fprintf(stderr,"Footnote block (%p) is %dpts high (%d lines).\n",
	      &footnote_paragraph,
	      footnotes_height,temp.line_count);
    }
    if (baseline_y>(page_height-bottom_margin)) {
      if (0)
	fprintf(stderr,"Breaking page %d at %.1fpts to make room for %dpt footnotes block\n",
		current_page,page_y,footnotes_height);
      break_page=1;
      page_penalty_add((baseline_y-(page_height-bottom_margin))
		       *OVERFULL_PAGE_PENALTY_PER_PT);
    }

    paragraph_clear(&temp);
    paragraph_clear(f); free(f);
  }

  // Does the line plus its cross-references require more space than there is?
  // - add height of cross-references for any verses in this line to height of
  // all cross-references and make sure that it can fit above the cross-references.
  if (isBodyParagraph) {
    int crossref_height=0;
    int crossref_para_count=crossref_count;
    int n,i;

    // Total height of crossrefs from previous lines on page
    for(n=0;n<crossref_count;n++)
      crossref_height+=crossrefs_queue[n]->total_height;

    // Now add height of cross refs on the current line(s) being drawn.
    for(n=line_num;n<=max_line_num;n++) {
      struct line_pieces *ll=p->paragraph_lines[n];
      for(i=0;i<ll->piece_count;i++)
	if (ll->pieces[i].crossrefs) {
	  crossref_height+=ll->pieces[i].crossrefs->total_height;
	  crossref_para_count++;
	}
    }

    if ((crossref_height+((crossref_para_count+1)*crossref_min_vspace))
	>(page_height-footnotes_total_height-bottom_margin-top_margin)) {
      if (0) {
	fprintf(stderr,"Breaking page %d at %.1fpts to avoid %dpts of cross references for %d verses (only %dpts available for crossrefs)\n",
		current_page,page_y,
		crossref_height+((crossref_para_count+1)*crossref_min_vspace),
		crossref_para_count,
		(page_height-footnotes_total_height-bottom_margin-top_margin));
	fprintf(stderr,"  page_height=%d, bottom_margin=%d, top_margin=%d\n",
		page_height,bottom_margin,top_margin);
	fprintf(stderr,"  footnotes_total_height=%dpts\n",footnotes_total_height);
	paragraph_dump(p);
      }
      break_page=1;
      page_penalty_add(((crossref_height+((crossref_para_count+1)*crossref_min_vspace))
			-(page_height-footnotes_total_height-bottom_margin-top_margin))
		       *OVERFULL_PAGE_PENALTY_PER_PT);
    } else {
      if (0) {
	fprintf(stderr,"%d cross reference blocks, totalling %dpts high (lines %d..%d)\n",
		crossref_para_count,
		crossref_height+((crossref_para_count+1)*crossref_min_vspace),
		p->first_crossref_line,max_line_num);
	crossref_queue_dump("queued crossreferences");
      }
    }
  }

  if (break_page) {
    // fprintf(stderr,"Page would over fill\n");
    page_penalty_add(OVERFULL_PAGE_PENALTY_PER_PT*20);
  }
  
  // convert y to libharu coordinate system (y=0 is at the bottom,
  // and the y position is the base-line of the text to render).
  // Don't apply line_spacing to adjustment, so that extra line spacing
  // appears below the line, rather than above it.
  float y=(page_height-page_y)-l->line_height;

  int i;
  float linegap=0;

  line_remove_trailing_space(l);

  if (l->alignment==AL_JUSTIFIED) line_remove_leading_space(l);
  line_recalculate_width(l);


  // Add extra spaces to justified lines, except for the last
  // line of a paragraph, and poetry lines.
  if (l->alignment==AL_JUSTIFIED) {
    if (p->line_count>(line_num+1)) {

      float points_to_add
	=l->max_line_width-l->line_width_so_far;
      
      if (points_to_add>0) {
	int elastic_pieces=0;
	for(i=0;i<l->piece_count;i++)
	  if (l->pieces[i].piece_is_elastic) elastic_pieces++;
	if (elastic_pieces) {
	  float slice=points_to_add/elastic_pieces;
	  for(i=0;i<l->piece_count;i++)
	    if (l->pieces[i].piece_is_elastic) l->pieces[i].piece_width+=slice;
	  l->line_width_so_far=l->max_line_width;
	}
      }
    }
  }

  {
    // Now draw the pieces
    l->on_page_y=page_y;
    if (drawingPage) {
      HPDF_Page_BeginText (page);
      HPDF_Page_SetTextRenderingMode (page, HPDF_FILL);
    }
    float x=0;
    switch(l->alignment) {
    case AL_LEFT: case AL_JUSTIFIED: case AL_NONE:
      // Finally apply any left margin that has been set
      x+=l->left_margin;
      break;
    case AL_CENTRED:
      x=(l->max_line_width-l->line_width_so_far)/2;
      break;
    case AL_RIGHT:
      x=l->max_line_width-l->line_width_so_far;
      break;
    }
    x-=l->left_hang;

    for(i=0;i<l->piece_count;i++) {
      if (drawingPage) {
	HPDF_Page_SetFontAndSize(page,l->pieces[i].font->font,l->pieces[i].actualsize);
	HPDF_Page_SetRGBFill(page,l->pieces[i].font->red,l->pieces[i].font->green,l->pieces[i].font->blue);
	HPDF_Page_TextOut(page,left_margin+x,y-l->pieces[i].piece_baseline,
			  l->pieces[i].piece);
	record_text(l->pieces[i].font,l->pieces[i].actualsize,
		    l->pieces[i].piece,left_margin+x,y-l->pieces[i].piece_baseline,0);
      }
      x=x+l->pieces[i].piece_width;
      // Don't adjust line gap for dropchars
      if (l->pieces[i].font->line_count==1) {
	if (l->pieces[i].font->linegap>linegap) linegap=l->pieces[i].font->linegap;
      }

      // Queue cross-references
      if (l->pieces[i].crossrefs) crossref_queue(l->pieces[i].crossrefs,page_y);
 
      if (!strcmp(l->pieces[i].font->font_nickname,"versenum"))
	last_verse_on_page=atoi(l->pieces[i].piece);
      if (!strcmp(l->pieces[i].font->font_nickname,"chapternum"))
	last_chapter_on_page=atoi(l->pieces[i].piece);      
    }
    if (drawingPage) HPDF_Page_EndText (page);
    if (!l->piece_count) linegap=l->line_height;

    // Indicate the height of each line
    if (debug_vspace&&drawingPage) {
      debug_vspace_x^=8;
      HPDF_Page_SetRGBFill (page, 0.0,0.0,0.0);
      HPDF_Page_Rectangle(page,
			  32+debug_vspace_x, y,
			  8,linegap*line_spacing);
      HPDF_Page_Fill(page);
    }
  }

  page_y=page_y+linegap*line_spacing;
    
  return 0;
}

int line_remove_trailing_space(struct line_pieces *l)
{
  // Remove any trailing spaces from the line
  int i;
  for(i=l->piece_count-1;i>=0;i--) {
    if (0) fprintf(stderr,"Considering piece #%d/%d '%s'\n",i,
		   l->piece_count,
		   l->pieces[i].piece);
    if ((!strcmp(" ",l->pieces[i].piece))
	||(!strcmp("",l->pieces[i].piece))) {
      l->piece_count=i;
      l->line_width_so_far-=l->pieces[i].piece_width;
      free(l->pieces[i].piece); l->pieces[i].piece=NULL;
      // fprintf(stderr,"  Removed trailing space from line\n");
    } else break;
  }
  return 0;
}

int line_remove_leading_space(struct line_pieces *l)
{
  // Remove any trailing spaces from the line
  int i,j;
  for(i=0;i<l->piece_count;i++) {
    if (0)
      fprintf(stderr,"Considering piece #%d/%d '%s'\n",i,
	      l->piece_count,
	      l->pieces[i].piece);
    if ((strcmp(" ",l->pieces[i].piece))&&(strcmp("",l->pieces[i].piece))) break;
    else {
      // fprintf(stderr,"  removing space from start of line.\n");
      free(l->pieces[i].piece); l->pieces[i].piece=NULL;
      l->line_width_so_far-=l->pieces[i].piece_width;
    }
  }

  if (i) {
    // Shuffle remaining pieces down
    // fprintf(stderr,"Shuffling remaining pieces down.\n");
    for(j=0;j<l->piece_count-i;j++) {
      bcopy(&l->pieces[j+i],&l->pieces[j],sizeof(struct piece));
    }
    
    l->piece_count-=i;
    
    // fprintf(stderr,"  Removed %d leading spaces from line\n",i);
  }
  return 0;
}

int line_dump_segment(struct line_pieces *l,int start,int end)
{
  int i;
  fprintf(stderr,"line_uid #%d: ",l->line_uid);
  if (l->left_margin) fprintf(stderr,"%+d ",l->left_margin);
  for(i=start;i<end;i++) {
    if (i&&(l->pieces[i-1].piece_width!=l->pieces[i-1].natural_width))
      fprintf(stderr,"%.1f",l->pieces[i-1].piece_width-l->pieces[i-1].natural_width);
    fprintf(stderr,"[%s]",l->pieces[i].piece);
  }
  fprintf(stderr,"\n");
  return 0;
}

int line_dump(struct line_pieces *l)
{
  if (l) return line_dump_segment(l,0,l->piece_count);    
  return 0;
}

int line_set_checkpoint(struct line_pieces *l)
{
  if (!l) return 0;
  if (!l->piece_count) return 0;
  
  // Start with checkpoint at end of current line.
  l->checkpoint=l->piece_count;
  while(l->checkpoint>0) {
    // move back one if the previous word is a verse number
    if (!strcasecmp(l->pieces[l->checkpoint-1].font->font_nickname,"versenum"))
      l->checkpoint--;
    // Or if we are drawing a footnote mark
    else if (!strcasecmp(current_font->font_nickname,"footnotemark"))
      l->checkpoint--;
    else if (!strcasecmp(current_font->font_nickname,"footnotemarkinfootnote"))
      l->checkpoint--;
    else if (!strcasecmp(current_font->font_nickname,"footnoteversenum"))
      l->checkpoint--;
    // Or if we see a non-breaking space
    else if (((unsigned char)l->pieces[l->checkpoint-1].piece[0])==0xa0)
      l->checkpoint-=2;
    else
      break;
  }
  if (l->checkpoint<0) l->checkpoint=0;
  // fprintf(stderr,"Set checkpoint at #%d in ",l->checkpoint);
  // line_dump(l);
  return 0;
}

int line_append_piece(struct line_pieces *l,struct piece *p)
{
  bcopy(p,&l->pieces[l->piece_count],sizeof(struct piece));
  l->pieces[l->piece_count++].piece=strdup(p->piece);
  return 0;
}

struct piece *new_line_piece(char *text,struct type_face *current_font,
			     float size,float text_width,
			     struct paragraph *crossrefs,float baseline,
			     int nobreak)
{
  struct piece *p=calloc(sizeof(struct piece),1);
  p->piece=strdup(text);
  p->font=current_font;
  p->actualsize=size;
  p->piece_width=text_width;
  p->natural_width=text_width;
  p->crossrefs=crossrefs;
  p->nobreak=nobreak;
  // only spaces (including non-breaking ones) are elastic
  if ((text[0]!=0x20)&&(((unsigned char)text[0])!=0xa0))
    p->piece_is_elastic=0;
  else {
    p->piece_is_elastic=1;
  }
  p->piece_baseline=baseline;  
  return p;
}
