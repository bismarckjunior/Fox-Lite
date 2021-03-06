/********************************************************************************
*                                                                               *
*                   M u l t i - L i n e   T e x t   W i d g e t                 *
*                                                                               *
*********************************************************************************
* Copyright (C) 1998,2018 by Jeroen van der Zijp.   All Rights Reserved.        *
*********************************************************************************
* This library is free software; you can redistribute it and/or modify          *
* it under the terms of the GNU Lesser General Public License as published by   *
* the Free Software Foundation; either version 3 of the License, or             *
* (at your option) any later version.                                           *
*                                                                               *
* This library is distributed in the hope that it will be useful,               *
* but WITHOUT ANY WARRANTY; without even the implied warranty of                *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                 *
* GNU Lesser General Public License for more details.                           *
*                                                                               *
* You should have received a copy of the GNU Lesser General Public License      *
* along with this program.  If not, see <http://www.gnu.org/licenses/>          *
********************************************************************************/
#include "xincs.h"
#include "fxver.h"
#include "fxdefs.h"
#include "fxmath.h"
#include "fxkeys.h"
#include "fxascii.h"
#include "fxunicode.h"
#include "FXColors.h"
#include "FXArray.h"
#include "FXHash.h"
#include "FXMutex.h"
#include "FXStream.h"
#include "FXString.h"
#include "FXElement.h"
#include "FXException.h"
#include "FXRex.h"
#include "FXSize.h"
#include "FXPoint.h"
#include "FXRectangle.h"
#include "FXObject.h"
#include "FXStringDictionary.h"
#include "FXSettings.h"
#include "FXRegistry.h"
#include "FXAccelTable.h"
#include "FXFont.h"
#include "FXEvent.h"
#include "FXWindow.h"
#include "FXDCWindow.h"
#include "FXApp.h"
#include "FXGIFIcon.h"
#include "FXScrollBar.h"
#include "FXText.h"
#include "FXComposeContext.h"
#include "icons.h"



/*
  Notes:
  - Line start array is one longer than number of visible lines.
  - Control characters in the buffer are OK (e.g. ^L).
  - Wrapped lines contain at least 1 character.
  - Generally, assume the following definitions in terms of how things work:

    position    Character position in the buffer; should avoid pointing to
                places other than the start of a UTF8 character.
    indent      logical character-index (not byte index) from the start of a line.
    line        A newline terminated sequence of characters. A line may be wrapped
                to multiple rows on the screen.
    row         Sequence of characters wrapped at the wrap-margin, therefore not
                necessarily ending at a newline
    column      Logical column from start of the line.


  - Buffer layout:

    Content  :  A  B  C  .  .  .  .  .  .  .  .  D  E  F  G
    Position :  0  1  2                          3  4  5  6    length=7
    Addresss :  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14    buffersize=7+11-3=15
                         ^                       ^
                         |                       |
                         gapstart=3              gapend=11     gaplen=11-3=8

    The gap is moved around the buffer so newly added text can be entered into the gap;
    when the gap becomes too small, the buffer is resized.
    This gapped-buffer technique minimizes the number of resizes of the buffer, and
    minimizes the number of block moves.

    The tail end of the visrows array will look like:

    visrow[0]= 0: "Q R S T U V W \n"
    visrow[1]= 8: "X Y Z"
    visrow[2]=11: <no text>
    visrow[3]=11: <no text>            length = 11

    The last legal position is length = 11.

  - While resizing window, keep track of a position which should remain visible, i.e.
    toppos=rowStart(position).  The position is changed same as toppos, except during
    resize.
  - When changing text, if we're looking at the tail end of the buffer, avoid jumping
    the top lines when the content hight shrinks.
  - Need to allow for one single routine to update style buffer same as text buffer
  - Maybe put all keyboard bindings into accelerator table.
  - Italic fonts are bit problematic on border between selected/unselected text
    due to kerning.
  - Perhaps split off buffer management into separate text buffer class (allows for
    multiple views).
  - Improve book keeping based on line/column numbers, not rows/characters.
  - If there is a style table, the style buffer is used as index into the style table,
    allowing for up to 255 styles (style index==0 is the default style).
    The style member in the FXHiliteStyle struct is used for underlining, strikeouts,
    and other effects.
    If there is NO style table but there is a style buffer, the style buffer can still
    be used for underlining, strikeouts, and other effects.
  - Sending SEL_CHANGED is pretty useless; should only be sent AFTER text change,
    and void* should contain some sensible info.
  - When in overstrike mode and having a selection, entering a character should
    replace the selection, not delete the selection and then overstrike the character
    after the selection.
  - When pasting or dropping whole lines, insert at begin of line instead of at cursor;
    question:- how to know we're pasting whole lines?
  - Need block cursor when in overstrike mode.
  - Inserting lots of stuff should show cursor.
  - Perhaps change text and style buffer to FXString for further complexity reduction.
  - Viewport definition:

        +------------------------------------------------+<-- 0
        |                                                |
        +----+--------------------------------------+----+<-- getVisibleY()
        |    |                                      |    |
        |    |           T e x t                    |    |
        |    |                                      |    |
        |    |                                      |    |
        +----+--------------------------------------+----+<-- getVisibleHeight()
        |                                                |
        +------------------------------------------------+<-- height
        ^    ^                                      ^    ^
        |    |                                      |    |
        0    |                                      |    width
           getVisibleX()             getVisibleWidth()

   - For now, right, top, and bottom bars are zero; subclasses may override
     and add space for text annotations.
   - Possible (minor) improvement to wrap(): don't break after space unless
     at least non-space was seen before that space.  This will cause a line
     to have at least some non-blank characters on it.
*/


#define MINSIZE         80              // Minimum gap size
#define NVISROWS        20              // Initial visible rows
#define MAXTABCOLUMNS   32              // Maximum tab column setting

#define TEXT_MASK       (TEXT_FIXEDWRAP|TEXT_WORDWRAP|TEXT_OVERSTRIKE|TEXT_READONLY|TEXT_NO_TABS|TEXT_AUTOINDENT|TEXT_SHOWACTIVE|TEXT_SHOWMATCH)

#define CC(x,in)        (((x)=='\t')?tabcolumns-in%tabcolumns:1)        // Count Columns

using namespace FX;

/*******************************************************************************/

namespace FX {


// Furnish our own version
extern FXAPI FXint __snprintf(FXchar* string,FXint length,const FXchar* format,...);


// Map
FXDEFMAP(FXText) FXTextMap[]={
  FXMAPFUNC(SEL_PAINT,0,FXText::onPaint),
  FXMAPFUNC(SEL_MOTION,0,FXText::onMotion),
  FXMAPFUNC(SEL_DRAGGED,0,FXText::onDragged),
  FXMAPFUNC(SEL_ENTER,0,FXText::onEnter),
  FXMAPFUNC(SEL_LEAVE,0,FXText::onLeave),
  FXMAPFUNC(SEL_TIMEOUT,FXText::ID_BLINK,FXText::onBlink),
  FXMAPFUNC(SEL_TIMEOUT,FXText::ID_FLASH,FXText::onFlash),
  FXMAPFUNC(SEL_TIMEOUT,FXText::ID_TIPTIMER,FXText::onTipTimer),
  FXMAPFUNC(SEL_TIMEOUT,FXText::ID_AUTOSCROLL,FXText::onAutoScroll),
  FXMAPFUNC(SEL_FOCUSIN,0,FXText::onFocusIn),
  FXMAPFUNC(SEL_FOCUSOUT,0,FXText::onFocusOut),
  FXMAPFUNC(SEL_BEGINDRAG,0,FXText::onBeginDrag),
  FXMAPFUNC(SEL_ENDDRAG,0,FXText::onEndDrag),
  FXMAPFUNC(SEL_LEFTBUTTONPRESS,0,FXText::onLeftBtnPress),
  FXMAPFUNC(SEL_LEFTBUTTONRELEASE,0,FXText::onLeftBtnRelease),
  FXMAPFUNC(SEL_MIDDLEBUTTONPRESS,0,FXText::onMiddleBtnPress),
  FXMAPFUNC(SEL_MIDDLEBUTTONRELEASE,0,FXText::onMiddleBtnRelease),
  FXMAPFUNC(SEL_RIGHTBUTTONPRESS,0,FXText::onRightBtnPress),
  FXMAPFUNC(SEL_RIGHTBUTTONRELEASE,0,FXText::onRightBtnRelease),
  FXMAPFUNC(SEL_UNGRABBED,0,FXText::onUngrabbed),
  FXMAPFUNC(SEL_DND_ENTER,0,FXText::onDNDEnter),
  FXMAPFUNC(SEL_DND_LEAVE,0,FXText::onDNDLeave),
  FXMAPFUNC(SEL_DND_DROP,0,FXText::onDNDDrop),
  FXMAPFUNC(SEL_DND_MOTION,0,FXText::onDNDMotion),
  FXMAPFUNC(SEL_DND_REQUEST,0,FXText::onDNDRequest),
  FXMAPFUNC(SEL_SELECTION_LOST,0,FXText::onSelectionLost),
  FXMAPFUNC(SEL_SELECTION_GAINED,0,FXText::onSelectionGained),
  FXMAPFUNC(SEL_SELECTION_REQUEST,0,FXText::onSelectionRequest),
  FXMAPFUNC(SEL_CLIPBOARD_LOST,0,FXText::onClipboardLost),
  FXMAPFUNC(SEL_CLIPBOARD_GAINED,0,FXText::onClipboardGained),
  FXMAPFUNC(SEL_CLIPBOARD_REQUEST,0,FXText::onClipboardRequest),
  FXMAPFUNC(SEL_KEYPRESS,0,FXText::onKeyPress),
  FXMAPFUNC(SEL_KEYRELEASE,0,FXText::onKeyRelease),
  FXMAPFUNC(SEL_QUERY_TIP,0,FXText::onQueryTip),
  FXMAPFUNC(SEL_QUERY_HELP,0,FXText::onQueryHelp),
  FXMAPFUNC(SEL_IME_START,0,FXText::onIMEStart),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_TOGGLE_EDITABLE,FXText::onUpdToggleEditable),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_TOGGLE_OVERSTRIKE,FXText::onUpdToggleOverstrike),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_CURSOR_ROW,FXText::onUpdCursorRow),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_CURSOR_COLUMN,FXText::onUpdCursorColumn),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_CUT_SEL,FXText::onUpdHaveEditableSelection),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_COPY_SEL,FXText::onUpdHaveSelection),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_PASTE_SEL,FXText::onUpdIsEditable),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_DELETE_SEL,FXText::onUpdHaveEditableSelection),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_REPLACE_SEL,FXText::onUpdHaveEditableSelection),
  FXMAPFUNC(SEL_UPDATE,FXText::ID_CLEAN_INDENT,FXText::onUpdHaveEditableSelection),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_TOP,FXText::onCmdCursorTop),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_BOTTOM,FXText::onCmdCursorBottom),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_HOME,FXText::onCmdCursorHome),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_END,FXText::onCmdCursorEnd),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_RIGHT,FXText::onCmdCursorRight),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_LEFT,FXText::onCmdCursorLeft),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_UP,FXText::onCmdCursorUp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_DOWN,FXText::onCmdCursorDown),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_PAGEUP,FXText::onCmdCursorPageUp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_PAGEDOWN,FXText::onCmdCursorPageDown),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_WORD_LEFT,FXText::onCmdCursorWordLeft),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_WORD_RIGHT,FXText::onCmdCursorWordRight),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_TOP,FXText::onCmdCursorShiftTop),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_BOTTOM,FXText::onCmdCursorShiftBottom),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_HOME,FXText::onCmdCursorShiftHome),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_END,FXText::onCmdCursorShiftEnd),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_RIGHT,FXText::onCmdCursorShiftRight),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_LEFT,FXText::onCmdCursorShiftLeft),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_UP,FXText::onCmdCursorShiftUp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_DOWN,FXText::onCmdCursorShiftDown),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_PAGEUP,FXText::onCmdCursorShiftPageUp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_PAGEDOWN,FXText::onCmdCursorShiftPageDown),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_WORD_LEFT,FXText::onCmdCursorShiftWordLeft),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_SHIFT_WORD_RIGHT,FXText::onCmdCursorShiftWordRight),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SCROLL_UP,FXText::onCmdScrollUp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SCROLL_DOWN,FXText::onCmdScrollDown),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SCROLL_TOP,FXText::onCmdScrollTop),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SCROLL_BOTTOM,FXText::onCmdScrollBottom),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SCROLL_CENTER,FXText::onCmdScrollCenter),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_STRING,FXText::onCmdInsertString),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_NEWLINE,FXText::onCmdInsertNewline),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_NEWLINE_ONLY,FXText::onCmdInsertNewlineOnly),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_NEWLINE_INDENT,FXText::onCmdInsertNewlineIndent),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_TAB,FXText::onCmdInsertTab),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_HARDTAB,FXText::onCmdInsertHardTab),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_INSERT_SOFTTAB,FXText::onCmdInsertSoftTab),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CUT_SEL,FXText::onCmdCutSel),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_COPY_SEL,FXText::onCmdCopySel),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DELETE_SEL,FXText::onCmdDeleteSel),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_REPLACE_SEL,FXText::onCmdReplaceSel),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_PASTE_SEL,FXText::onCmdPasteSel),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_PASTE_MIDDLE,FXText::onCmdPasteMiddle),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SELECT_CHAR,FXText::onCmdSelectChar),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SELECT_WORD,FXText::onCmdSelectWord),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SELECT_LINE,FXText::onCmdSelectLine),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SELECT_ALL,FXText::onCmdSelectAll),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DESELECT_ALL,FXText::onCmdDeselectAll),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_BACKSPACE_CHAR,FXText::onCmdBackspaceChar),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_BACKSPACE_WORD,FXText::onCmdBackspaceWord),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_BACKSPACE_BOL,FXText::onCmdBackspaceBol),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DELETE_CHAR,FXText::onCmdDeleteChar),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DELETE_WORD,FXText::onCmdDeleteWord),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DELETE_EOL,FXText::onCmdDeleteEol),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DELETE_ALL,FXText::onCmdDeleteAll),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_DELETE_LINE,FXText::onCmdDeleteLine),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_TOGGLE_EDITABLE,FXText::onCmdToggleEditable),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_TOGGLE_OVERSTRIKE,FXText::onCmdToggleOverstrike),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_ROW,FXText::onCmdCursorRow),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CURSOR_COLUMN,FXText::onCmdCursorColumn),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SETSTRINGVALUE,FXText::onCmdSetStringValue),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_GETSTRINGVALUE,FXText::onCmdGetStringValue),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_UPPER_CASE,FXText::onCmdChangeCase),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_LOWER_CASE,FXText::onCmdChangeCase),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_JOIN_LINES,FXText::onCmdJoinLines),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_GOTO_MATCHING,FXText::onCmdGotoMatching),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SELECT_MATCHING,FXText::onCmdSelectMatching),
  FXMAPFUNCS(SEL_COMMAND,FXText::ID_SELECT_BRACE,FXText::ID_SELECT_ANG,FXText::onCmdSelectBlock),
  FXMAPFUNCS(SEL_COMMAND,FXText::ID_LEFT_BRACE,FXText::ID_LEFT_ANG,FXText::onCmdBlockBeg),
  FXMAPFUNCS(SEL_COMMAND,FXText::ID_RIGHT_BRACE,FXText::ID_RIGHT_ANG,FXText::onCmdBlockEnd),
  FXMAPFUNCS(SEL_COMMAND,FXText::ID_SHIFT_LEFT,FXText::ID_SHIFT_TABRIGHT,FXText::onCmdShiftText),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_COPY_LINE,FXText::onCmdCopyLine),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_MOVE_LINE_UP,FXText::onCmdMoveLineUp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_MOVE_LINE_DOWN,FXText::onCmdMoveLineDown),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_CLEAN_INDENT,FXText::onCmdShiftText),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SETHELPSTRING,FXText::onCmdSetHelp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_GETHELPSTRING,FXText::onCmdGetHelp),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_SETTIPSTRING,FXText::onCmdSetTip),
  FXMAPFUNC(SEL_COMMAND,FXText::ID_GETTIPSTRING,FXText::onCmdGetTip),
  };


// Object implementation
FXIMPLEMENT(FXText,FXScrollArea,FXTextMap,ARRAYNUMBER(FXTextMap))


// Delimiters
const FXchar FXText::textDelimiters[]="~.,/\\`'!@#$%^&*()-=+{}|[]\":;<>?";

// Matching things
static const FXchar lefthand[]="{[(<";
static const FXchar righthand[]="}])>";

// Spaces, lots of spaces
static const FXchar spaces[MAXTABCOLUMNS+1]="                                ";

/*******************************************************************************/


// For deserialization
FXText::FXText(){
  flags|=FLAG_ENABLED|FLAG_DROPTARGET;
  buffer=NULL;
  sbuffer=NULL;
  visrows=NULL;
  length=0;
  nvisrows=0;
  nrows=1;
  gapstart=0;
  gapend=0;
  toppos=0;
  toprow=0;
  keeppos=0;
  select.startpos=0;
  select.endpos=-1;
  select.startcol=0;
  select.endcol=-1;
  hilite.startpos=0;
  hilite.endpos=-1;
  hilite.startcol=0;
  hilite.endcol=-1;
  anchorpos=0;
  anchorrow=0;
  anchorcol=0;
  anchorvcol=0;
  cursorpos=0;
  cursorrow=0;
  cursorcol=0;
  cursorvcol=0;
  prefcol=-1;
  margintop=0;
  marginbottom=0;
  marginleft=0;
  marginright=0;
  wrapwidth=80;
  wrapcolumns=80;
  tabwidth=8;
  tabcolumns=8;
  barwidth=0;
  barcolumns=0;
  font=NULL;
  textColor=0;
  selbackColor=0;
  seltextColor=0;
  hilitebackColor=0;
  hilitetextColor=0;
  activebackColor=0;
  numberColor=0;
  cursorColor=0;
  barColor=0;
  textWidth=0;
  textHeight=0;
  delimiters=textDelimiters;
  vrows=0;
  vcols=0;
  hilitestyles=NULL;
  blink=FLAG_CARET;
  matchtime=0;
  grabx=0;
  graby=0;
  mode=MOUSE_NONE;
  modified=false;
  }


// Text widget
FXText::FXText(FXComposite *p,FXObject* tgt,FXSelector sel,FXuint opts,FXint x,FXint y,FXint w,FXint h,FXint pl,FXint pr,FXint pt,FXint pb):FXScrollArea(p,opts,x,y,w,h){
  flags|=FLAG_ENABLED|FLAG_DROPTARGET;
  target=tgt;
  message=sel;
  callocElms(buffer,MINSIZE);
  sbuffer=NULL;
  callocElms(visrows,NVISROWS+1);
  length=0;
  nrows=1;
  nvisrows=NVISROWS;
  gapstart=0;
  gapend=MINSIZE;
  toppos=0;
  toprow=0;
  keeppos=0;
  select.startpos=0;
  select.endpos=-1;
  select.startcol=0;
  select.endcol=-1;
  hilite.startpos=0;
  hilite.endpos=-1;
  hilite.startcol=0;
  hilite.endcol=-1;
  anchorpos=0;
  anchorrow=0;
  anchorcol=0;
  anchorvcol=0;
  cursorpos=0;
  cursorrow=0;
  cursorcol=0;
  cursorvcol=0;
  prefcol=-1;
  margintop=pt;
  marginbottom=pb;
  marginleft=pl;
  marginright=pr;
  wrapwidth=80;
  wrapcolumns=80;
  tabwidth=8;
  tabcolumns=8;
  barwidth=0;
  barcolumns=0;
  font=getApp()->getNormalFont();
  hilitestyles=NULL;
  blink=FLAG_CARET;
  defaultCursor=getApp()->getDefaultCursor(DEF_TEXT_CURSOR);
  dragCursor=getApp()->getDefaultCursor(DEF_TEXT_CURSOR);
  textColor=getApp()->getForeColor();
  selbackColor=getApp()->getSelbackColor();
  seltextColor=getApp()->getSelforeColor();
  hilitebackColor=FXRGB(255,128,128);
  hilitetextColor=getApp()->getForeColor();
  activebackColor=backColor;
  numberColor=textColor;
  cursorColor=getApp()->getForeColor();
  barColor=backColor;
  textWidth=0;
  textHeight=0;
  delimiters=textDelimiters;
  vrows=0;
  vcols=0;
  matchtime=0;
  grabx=0;
  graby=0;
  mode=MOUSE_NONE;
  modified=false;
  }


// If window can have focus
FXbool FXText::canFocus() const {
  return true;
  }


// Into focus chain
void FXText::setFocus(){
  FXScrollArea::setFocus();
  setDefault(true);
  flags&=~FLAG_UPDATE;
  if(getApp()->hasInputMethod()){
    createComposeContext();
    getComposeContext()->setFont(font);
    getComposeContext()->focusIn();
    }
  }


// Out of focus chain
void FXText::killFocus(){
  FXScrollArea::killFocus();
  setDefault(maybe);
  flags|=FLAG_UPDATE;
  if(getApp()->hasInputMethod()){
    destroyComposeContext();
    }
  }


// Enable the window
void FXText::enable(){
  if(!(flags&FLAG_ENABLED)){
    FXScrollArea::enable();
    update();
    }
  }


// Disable the window
void FXText::disable(){
  if(flags&FLAG_ENABLED){
    FXScrollArea::disable();
    update();
    }
  }


// Create window
void FXText::create(){
  FXScrollArea::create();
  font->create();
  tabwidth=tabcolumns*font->getTextWidth(" ",1);
  barwidth=barcolumns*font->getTextWidth("8",1);
  recalc();
  }


// Detach window
void FXText::detach(){
  FXScrollArea::detach();
  font->detach();
  }

/*******************************************************************************/

// Move the gap; gap is never moved inside utf character
void FXText::movegap(FXint pos){
  register FXint gaplen=gapend-gapstart;
  FXASSERT(0<=pos && pos<=length);
  FXASSERT(0<=gapstart && gapstart<=length);
  if(gapstart<pos){
    moveElms(&buffer[gapstart],&buffer[gapend],pos-gapstart);
    if(sbuffer){moveElms(&sbuffer[gapstart],&sbuffer[gapend],pos-gapstart);}
    gapend=pos+gaplen;
    gapstart=pos;
    }
  else if(pos<gapstart){
    moveElms(&buffer[pos+gaplen],&buffer[pos],gapstart-pos);
    if(sbuffer){moveElms(&sbuffer[pos+gaplen],&sbuffer[pos],gapstart-pos);}
    gapend=pos+gaplen;
    gapstart=pos;
    }
  }


// Size gap
void FXText::sizegap(FXint sz){
  register FXint gaplen=gapend-gapstart;
  FXASSERT(0<=gapstart && gapstart<=length);
  if(sz>=gaplen){
    sz+=MINSIZE;
    if(!resizeElms(buffer,length+sz)){
      fxerror("%s::sizegap: out of memory.\n",getClassName());
      }
    moveElms(&buffer[gapstart+sz],&buffer[gapend],length-gapstart);
    if(sbuffer){
      if(!resizeElms(sbuffer,length+sz)){
        fxerror("%s::sizegap: out of memory.\n",getClassName());
        }
      moveElms(&sbuffer[gapstart+sz],&sbuffer[gapend],length-gapstart);
      }
    gapend=gapstart+sz;
    }
  }


// Squeeze out the gap by moving it to the end of the buffer
void FXText::squeezegap(){
  if(gapstart!=length){
    moveElms(&buffer[gapstart],&buffer[gapend],length-gapstart);
    if(sbuffer){moveElms(&sbuffer[gapstart],&sbuffer[gapend],length-gapstart);}
    gapend=length+gapend-gapstart;
    gapstart=length;
    }
  }

/*******************************************************************************/

// Make a valid position, at the start of a wide character
FXint FXText::validPos(FXint pos) const {
  const FXchar* ptr=&buffer[(gapend-gapstart)&((~pos+gapstart)>>31)];
  if(pos<=0) return 0;
  if(pos>=length) return length;
  return (FXISUTF8(ptr[pos]) || --pos<=0 || FXISUTF8(ptr[pos]) || --pos<=0 || FXISUTF8(ptr[pos]) || --pos), pos;
  }


// Decrement; a wide character does not cross the gap, so if pos is at
// or below below the gap, we read from the segment below the gap
FXint FXText::dec(FXint pos) const {
  const FXchar* ptr=&buffer[(gapend-gapstart)&((gapstart-pos)>>31)];
  return (--pos<=0 || FXISUTF8(ptr[pos]) || --pos<=0 || FXISUTF8(ptr[pos]) || --pos<=0 || FXISUTF8(ptr[pos]) || --pos), pos;
  }


// Increment; since a wide character does not cross the gap, if we
// start under the gap the last character accessed is below the gap
FXint FXText::inc(FXint pos) const {
  const FXchar* ptr=&buffer[(gapend-gapstart)&((~pos+gapstart)>>31)];
  return (++pos>=length || FXISUTF8(ptr[pos]) || ++pos>=length || FXISUTF8(ptr[pos]) || ++pos>=length || FXISUTF8(ptr[pos]) || ++pos), pos;
  }

/*******************************************************************************/

// Get byte
FXint FXText::getByte(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  return (FXuchar)buffer[pos+((gapend-gapstart)&((~pos+gapstart)>>31))];
  }


// Get character, assuming that gap never inside utf8 encoding
FXwchar FXText::getChar(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  const FXuchar* ptr=(FXuchar*)&buffer[pos+((gapend-gapstart)&((~pos+gapstart)>>31))];
  FXwchar w=ptr[0];
  if(__unlikely(0xC0<=w)){ w=(w<<6)^ptr[1]^0x3080;
  if(__unlikely(0x800<=w)){ w=(w<<6)^ptr[2]^0x20080;
  if(__unlikely(0x10000<=w)){ w=(w<<6)^ptr[3]^0x400080; }}}
  return w;
  }


// Get length of wide character at position pos
FXint FXText::getCharLen(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  return FXUTF8LEN(buffer[pos+((gapend-gapstart)&((~pos+gapstart)>>31))]);
  }


// Get style
FXint FXText::getStyle(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  return (FXuchar)sbuffer[pos+((gapend-gapstart)&((~pos+gapstart)>>31))];
  }

/*******************************************************************************/

// Its a little bit more complex than this:
// We need to deal with diacritics, i.e. non-spacing stuff.  When wrapping, scan till
// the next starter-character [the one with charCombining(c)==0].  Then measure the
// string from that point on. This means FXFont::getCharWidth() is really quite useless.
// Next, we also have the issue of ligatures [fi, AE] and kerning-pairs [VA].
// With possible kerning pairs, we should really measure stuff from the start of the
// line [but this is *very* expensive!!].  We may want to just back up a few characters;
// perhaps to the start of the word, or just the previous character, if not a space.
// Need to investigate this some more; for now assume Normalization Form C.

// Character width
FXint FXText::charWidth(FXwchar ch,FXint indent) const {
  if(__likely(' '<=ch)) return font->getCharWidth(ch);
  if(__likely(ch=='\t')) return (tabwidth-indent%tabwidth);
  return font->getCharWidth('^')+font->getCharWidth(ch|0x40);
  }


// Calculate X offset from line start to pos
FXint FXText::xoffset(FXint start,FXint pos) const {
  register FXint w=0;
  FXASSERT(0<=start && start<=pos && pos<=length);
  while(start<pos){
    w+=charWidth(getChar(start),w);
    start+=getCharLen(start);
    }
  return w;
  }


// Start of next wrapped line
// Position returned is start of next line, i.e. after newline
// or after space where line got broken during line wrapping.
FXint FXText::wrap(FXint start) const {
  register FXint lw,cw,p,s;
  register FXwchar ch;
  FXASSERT(0<=start && start<=length);
  lw=0;
  p=s=start;
  while(p<length){
    ch=getChar(p);
    if(ch=='\n') return p+1;            // Newline always breaks
    cw=charWidth(ch,lw);
    if(lw+cw>wrapwidth){                // Technically, a tab-before-wrap should be as wide as space!
      if(s>start) return s;             // We remembered the last space we encountered; break there!
      if(p>start) return p;             // Got at least one character, so return that
      return p+getCharLen(p);           // Otherwise, advance one extra character
      }
    lw+=cw;
    p+=getCharLen(p);
    if(Unicode::isSpace(ch)) s=p;       // Remember potential break point!
    }
  return length;
  }

/*******************************************************************************/

// Return position of begin of paragraph
FXint FXText::lineStart(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  while(0<pos && getByte(pos-1)!='\n'){
    pos--;
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }


// Return position of end of paragraph
FXint FXText::lineEnd(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  while(pos<length && getByte(pos)!='\n'){
    pos++;
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }


// Return start of next line
FXint FXText::nextLine(FXint pos,FXint nl) const {
  FXASSERT(0<=pos && pos<=length);
  if(0<nl){
    while(pos<length){
      if(getByte(pos++)=='\n' && --nl<=0) break;
      }
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }


// Return start of previous line
FXint FXText::prevLine(FXint pos,FXint nl) const {
  FXASSERT(0<=pos && pos<=length);
  if(0<nl){
    while(0<pos){
      if(getByte(pos-1)=='\n' && --nl<0) break;
      pos--;
      }
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }

/*******************************************************************************/

// Return row start
FXint FXText::rowStart(FXint pos) const {
  register FXint p,t;
  FXASSERT(0<=pos && pos<=length);
  if(options&TEXT_WORDWRAP){
    p=pos;
    while(0<pos && getByte(pos-1)!='\n'){               // Find line start first
      pos--;
      }
    while(pos<p && (t=wrap(pos))<=p && t<length){       // Find row containing position, except if last row
      pos=t;
      }
    }
  else{
    while(0<pos && getByte(pos-1)!='\n'){               // Find line start
      pos--;
      }
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }


// Return row end
FXint FXText::rowEnd(FXint pos) const {
  register FXint p,t;
  FXASSERT(0<=pos && pos<=length);
  if(options&TEXT_WORDWRAP){
    p=pos;
    while(0<pos && getByte(pos-1)!='\n'){               // Find line start first
      pos--;
      }
    while(pos<=p && pos<length){                        // Find row past position
      pos=wrap(pos);
      }
    if(p<pos){                                          // Back off if line broke at space
      t=dec(pos);
      if(Unicode::isSpace(getChar(t))) pos=t;
      }
    }
  else{
    while(pos<length && getByte(pos)!='\n'){            // Hunt for end of line
      pos++;
      }
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }


// Move to next row given start of line
FXint FXText::nextRow(FXint pos,FXint nr) const {
  register FXint p,t;
  FXASSERT(0<=pos && pos<=length);
  if(0<nr){
    if(options&TEXT_WORDWRAP){
      p=pos;
      while(0<pos && getByte(pos-1)!='\n'){             // Find line start first
        pos--;
        }
      while(pos<p && (t=wrap(pos))<=p && t<length){     // Find row containing pos
        pos=t;
        }
      while(pos<length){                                // Then wrap until nth row after
        pos=wrap(pos);
        if(--nr<=0) break;
        }
      }
    else{
      while(pos<length){                                // Hunt for begin of nth next line
        if(getByte(pos++)=='\n' && --nr<=0) break;
        }
      }
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }


// Move to previous row given start of line
FXint FXText::prevRow(FXint pos,FXint nr) const {
  register FXint p,q,t;
  FXASSERT(0<=pos && pos<=length);
  if(0<nr){
    if(options&TEXT_WORDWRAP){
      while(0<pos){
        p=pos;
        while(0<pos && getByte(pos-1)!='\n'){           // Find line start first
          pos--;
          }
        FXASSERT(0<=pos);
        q=pos;
        while(q<p && (t=wrap(q))<=p && t<length){       // Decrement number of rows to this point
          nr--;
          q=t;
          }
        while(nr<0){                                    // Went too far forward; try again from pos
          pos=wrap(pos);
          nr++;
          }
        FXASSERT(0<=nr);
        if(nr==0) break;
        if(pos==0) break;
        pos--;                                          // Skip over newline
        nr--;                                           // Which also counts as a row
        }
      }
    else{
      while(0<pos){                                     // Find previous line start
        if(getByte(pos-1)=='\n' && --nr<0) break;
        pos--;
        }
      }
    }
  FXASSERT(0<=pos && pos<=length);
  return pos;
  }

/*******************************************************************************/

// Find row number from position
// If position falls in visible area, scan visrows for the proper row;
// otherwise, count rows from start of row containing position to the
// first visible line, or from the last visible line to the position.
FXint FXText::rowFromPos(FXint pos) const {
  FXint row=0;
  if(pos<visrows[0]){                                                   // Above visible buffer
    if(pos<=0) return 0;
    return toprow-countRows(rowStart(pos),visrows[0]);
    }
  if(visrows[nvisrows-1]<=pos && visrows[nvisrows-1]<visrows[nvisrows]){// Below visible buffer AND there are more lines
    if(pos>=length) return nrows-1;
    return toprow+nvisrows-1+countRows(visrows[nvisrows-1],pos);
    }
  while(row+1<nvisrows && visrows[row+1]<=pos && visrows[row]<visrows[row+1]) row++;
  FXASSERT(0<=row && row<nvisrows);
  FXASSERT(visrows[row]<=pos && pos<=visrows[row+1]);
  return toprow+row;
  }


// Find row start position from row number
// If row falls in visible area, we can directly return the row start position;
// otherwise, we scan backward from first visible line, or forward from last
// visible line, checking for start or end of buffer of course.
FXint FXText::posFromRow(FXint row) const {
  if(row<toprow){
    if(row<0) return 0;
    return prevRow(visrows[0],toprow-row);
    }
  if(row>=toprow+nvisrows){
    if(row>=nrows) return length;
    return nextRow(visrows[nvisrows-1],row-toprow-nvisrows+1);
    }
  return visrows[row-toprow];
  }


// Determine logical indent of position pos relative to line start.
// Stop at the end of the line (not row).
FXint FXText::columnFromPos(FXint start,FXint pos) const {
  FXint column=0; FXuchar c;
  FXASSERT(0<=start && pos<=length);
  while(start<pos && (c=getByte(start))!='\n'){
    column+=CC(c,column);
    start+=getCharLen(start);
    }
  return column;
  }


// Determine position of logical indent relative to line start.
// Stop at the end of the line (not row).
FXint FXText::posFromColumn(FXint start,FXint col) const {
  FXint column=0; FXuchar c;
  FXASSERT(0<=start && start<=length);
  while(start<length && (c=getByte(start))!='\n'){
    column+=CC(c,column);
    if(col<column) break;
    start+=getCharLen(start);
    }
  return start;
  }

/*******************************************************************************/

// Check if w is delimiter
FXbool FXText::isdelimiter(FXwchar w) const {
  FXchar wcs[5]={'\0','\0','\0','\0','\0'};
  if(__unlikely(128<=w)){
    wc2utf(wcs,w);
    return (strstr(delimiters,wcs)!=NULL);
    }
  return (strchr(delimiters,w)!=NULL);
  }


// Find end of previous word
FXint FXText::leftWord(FXint pos) const {
  register FXwchar ch;
  FXASSERT(0<=pos && pos<=length);
  if(0<pos){
    pos=dec(pos);
    ch=getChar(pos);
    if(isdelimiter(ch)){
      while(0<pos){
        ch=getChar(dec(pos));
        if(Unicode::isSpace(ch) || !isdelimiter(ch)) return pos;
        pos=dec(pos);
        }
      }
    else if(!Unicode::isSpace(ch)){
      while(0<pos){
        ch=getChar(dec(pos));
        if(Unicode::isSpace(ch) || isdelimiter(ch)) return pos;
        pos=dec(pos);
        }
      }
    while(0<pos){
      ch=getChar(dec(pos));
      if(!Unicode::isBlank(ch)) return pos;
      pos=dec(pos);
      }
    }
  return pos;
  }


// Find begin of next word
FXint FXText::rightWord(FXint pos) const {
  register FXwchar ch;
  FXASSERT(0<=pos && pos<=length);
  if(pos<length){
    ch=getChar(pos);
    pos=inc(pos);
    if(isdelimiter(ch)){
      while(pos<length){
        ch=getChar(pos);
        if(Unicode::isSpace(ch) || !isdelimiter(ch)) return pos;
        pos=inc(pos);
        }
      }
    else if(!Unicode::isSpace(ch)){
      while(pos<length){
        ch=getChar(pos);
        if(Unicode::isSpace(ch) || isdelimiter(ch)) return pos;
        pos=inc(pos);
        }
      }
    while(pos<length){
      ch=getChar(pos);
      if(!Unicode::isBlank(ch)) return pos;
      pos=inc(pos);
      }
    }
  return pos;
  }


// Find begin of a word
FXint FXText::wordStart(FXint pos) const {
  register FXwchar ch;
  FXASSERT(0<=pos && pos<=length);
  if(0<pos){
    ch=(pos<length)?getChar(pos):' ';
    if(ch=='\n') return pos;
    if(Unicode::isBlank(ch)){
      while(0<pos){
        ch=getChar(dec(pos));
        if(!Unicode::isBlank(ch)) return pos;
        pos=dec(pos);
        }
      }
    else if(isdelimiter(ch)){
      while(0<pos){
        ch=getChar(dec(pos));
        if(!isdelimiter(ch)) return pos;
        pos=dec(pos);
        }
      }
    else{
      while(0<pos){
        ch=getChar(dec(pos));
        if(isdelimiter(ch) || Unicode::isSpace(ch)) return pos;
        pos=dec(pos);
        }
      }
    }
  return pos;
  }


// Find end of word
FXint FXText::wordEnd(FXint pos) const {
  register FXwchar ch;
  FXASSERT(0<=pos && pos<=length);
  if(pos<length){
    ch=getChar(pos);
    if(ch=='\n') return pos+1;
    if(Unicode::isBlank(ch)){
      while(pos<length){
        ch=getChar(pos);
        if(!Unicode::isBlank(ch)) return pos;
        pos=inc(pos);
        }
      }
    else if(isdelimiter(ch)){
      while(pos<length){
        ch=getChar(pos);
        if(!isdelimiter(ch)) return pos;
        pos=inc(pos);
        }
      }
    else{
      while(pos<length){
        ch=getChar(pos);
        if(isdelimiter(ch) || Unicode::isSpace(ch)) return pos;
        pos=inc(pos);
        }
      }
    }
  return pos;
  }

/*******************************************************************************/

// Search forward for match
FXint FXText::matchForward(FXint pos,FXint end,FXwchar l,FXwchar r,FXint level) const {
  register FXwchar ch;
  FXASSERT(0<=end && end<=length);
  FXASSERT(0<=pos && pos<=length);
  while(pos<end){
    ch=getChar(pos);
    if(ch==r){
      level--;
      if(level<=0) return pos;
      }
    else if(ch==l){
      level++;
      }
    pos=inc(pos);
    }
  return -1;
  }


// Search backward for match
FXint FXText::matchBackward(FXint pos,FXint beg,FXwchar l,FXwchar r,FXint level) const {
  register FXwchar ch;
  FXASSERT(0<=beg && beg<=length);
  FXASSERT(0<=pos && pos<=length);
  while(beg<=pos){
    ch=getChar(pos);
    if(ch==l){
      level--;
      if(level<=0) return pos;
      }
    else if(ch==r){
      level++;
      }
    pos=dec(pos);
    }
  return -1;
  }


// Search for matching character
FXint FXText::findMatching(FXint pos,FXint beg,FXint end,FXwchar ch,FXint level) const {
  FXASSERT(0<=level);
  FXASSERT(0<=pos && pos<=length);
  switch(ch){
    case '{': return matchForward(pos+1,end,'{','}',level);
    case '}': return matchBackward(pos-1,beg,'{','}',level);
    case '[': return matchForward(pos+1,end,'[',']',level);
    case ']': return matchBackward(pos-1,beg,'[',']',level);
    case '(': return matchForward(pos+1,end,'(',')',level);
    case ')': return matchBackward(pos-1,beg,'(',')',level);
    }
  return -1;
  }


// Flash matching braces or parentheses
// If flashing briefly, highlight only if visible; otherwise, highlight always
void FXText::flashMatching(){
  killHighlight();
  getApp()->removeTimeout(this,ID_FLASH);
  if((options&TEXT_SHOWMATCH) && 0<cursorpos){
    FXint beg=(matchtime<forever) ? visrows[0] : 0;
    FXint end=(matchtime<forever) ? visrows[nvisrows] : length;
    FXint matchpos=findMatching(cursorpos-1,beg,end,getByte(cursorpos-1),1);
    if(0<=matchpos){
      setHighlight(matchpos,1);
      if(0<matchtime && matchtime<forever){
        getApp()->addTimeout(this,ID_FLASH,matchtime);
        }
      }
    }
  }

/*******************************************************************************/

// Count number of columns; start should be on a row start
FXint FXText::countCols(FXint start,FXint end) const {
  register FXint nc=0,in=0;
  register FXwchar ch;
  FXASSERT(0<=start && end<=length);
  while(start<end){
    ch=getChar(start);
    if(ch=='\n'){ start++; if(in>nc) nc=in; in=0; continue; }
    if(ch=='\t'){ start++; in+=(tabcolumns-nc%tabcolumns); continue; }
    start+=getCharLen(start);
    in++;
    }
  if(in>nc) nc=in;
  return nc;
  }


// Count number of rows; start should be on a row start
FXint FXText::countRows(FXint start,FXint end) const {
  FXint result=0;
  FXint p=start;
  FXint q=start;
  FXint s=start;
  FXint w=0;
  FXint cw;
  FXwchar c;
  FXASSERT(0<=start && end<=length+1);
  if(options&TEXT_WORDWRAP){
    while(p<end){
      c=getChar(p);
      if(c=='\n'){                    // Break at newline
        q=p=s=p+1;
        result++;
        w=0;
        continue;
        }
      cw=charWidth(c,w);
      if(w+cw>wrapwidth){             // Break due to wrap
        result++;
        w=0;
        if(q<s){                      // Break past last space seen
          p=q=s;
          continue;
          }
        if(p==q){                     // Always at least one character on each line!
          p+=getCharLen(p);
          }
        q=s=p;
        continue;
        }
      w+=cw;
      p+=getCharLen(p);
      if(Unicode::isSpace(c)) s=p;    // Remember potential break point!
      }
    }
  else{
    while(p<end){
      c=getByte(p++);
      if(c=='\n'){ q=p; result++; }
      }
    }
  result+=(q<p);        // Last line non-empty
  return result;
  }


// Count number of newlines
FXint FXText::countLines(FXint start,FXint end) const {
  FXint result=0;
  FXint p=start;
  FXint q=start;
  FXASSERT(0<=start && end<=length+1);
  while(p<end){
    if(getByte(p++)=='\n'){ q=p; result++; }
    }
  result+=(q<p);        // Last line non-empty
  return result;
  }

/*******************************************************************************/

// Measure lines; start and end should be on a row start
FXint FXText::measureText(FXint start,FXint end,FXint& wmax,FXint& hmax) const {
  FXint result=0;
  FXint p=start;
  FXint q=start;
  FXint s=start;
  FXint w=0;
  FXint cw;
  FXwchar c;
  FXASSERT(0<=start && end<=length+1);
  if(options&TEXT_WORDWRAP){
    wmax=wrapwidth;
    while(p<end){
      c=getChar(p);
      if(c=='\n'){                      // Break at newline
        q=s=p=p+1;
        result++;
        w=0;
        continue;
        }
      cw=charWidth(c,w);
      if(w+cw>wrapwidth){               // Break due to wrap
        result++;
        w=0;
        if(s>q){                        // Break past last space seen
          q=p=s;
          continue;
          }
        if(p==q){                       // Always at least one character on each line!
          p+=getCharLen(p);
          }
        q=s=p;
        continue;
        }
      w+=cw;
      p+=getCharLen(p);
      if(Unicode::isSpace(c)) s=p;      // Remember potential break point!
      }
    }
  else{
    wmax=0;
    while(p<end){
      c=getChar(p);
      if(c=='\n'){                      // Break at newline
        if(w>wmax) wmax=w;
        result++;
        q=++p;
        w=0;
        continue;
        }
      w+=charWidth(c,w);
      p+=getCharLen(p);
      }
    if(w>wmax) wmax=w;
    }
  result+=(q<p);                        // Last line non-empty
  hmax=result*font->getFontHeight();
  return result;
  }


// Recalculate line starts
void FXText::calcVisRows(FXint startline,FXint endline){
  register FXint pos;
  FXASSERT(nvisrows>0);
  startline=FXCLAMP(0,startline,nvisrows);
  endline=FXCLAMP(0,endline,nvisrows);
  if(startline<=endline){
    FXASSERT(0<=toppos && toppos<=length);
    if(startline==0){
      visrows[0]=toppos;
      startline=1;
      }
    pos=visrows[startline-1];
    if(options&TEXT_WORDWRAP){
      while(startline<=endline && pos<length){
        pos=wrap(pos);
        FXASSERT(0<=pos && pos<=length);
        visrows[startline++]=pos;
        }
      }
    else{
      while(startline<=endline && pos<length){
        pos=nextLine(pos);
        FXASSERT(0<=pos && pos<=length);
        visrows[startline++]=pos;
        }
      }
    while(startline<=endline){
      visrows[startline++]=length;
      }
    }
  }


// Recompute the text dimensions; this is based on font, margins, wrapping
// and line numbers, so if any of these things change it has to be redone.
void FXText::recompute(){
  FXint hh=font->getFontHeight();
  FXint ww1,hh1,ww2,hh2;

  // The keep position is where we want to have the top of the buffer be;
  // make sure this is still inside the text buffer!
  keeppos=FXCLAMP(0,keeppos,length);

  // Due to wrapping, toppos which USED to point to a row start may no
  // longer do so.  We back off till the nearest row start.  If we resize
  // the window repeatedly, toppos will not wander away indiscriminately.
  toppos=rowStart(keeppos);

  // Remeasure the text; first, the part above the visible buffer, then
  // the rest.  This avoids measuring the entire text twice, which is
  // quite expensive.
  toprow=measureText(0,toppos,ww1,hh1);
//  nrows=toprow+measureText(toppos,length,ww2,hh2);    // FIXME
//FXTRACE((100,"nrows (new): %d:\n",nrows));
  nrows=toprow+measureText(toppos,length+1,ww2,hh2);    // FIXME
//FXTRACE((100,"nrows (old): %d:\n",nrows));

  // The width of the buffer is the maximum of the chunks measured above,
  // while the height is their sum.
  textWidth=FXMAX(ww1,ww2);
  textHeight=hh1+hh2;

  // Adjust position, keeping same fractional position. Do this AFTER having
  // determined toprow, which may have changed due to wrapping changes.
  pos_y=-toprow*hh-(-pos_y%hh);

  FXTRACE((150,"recompute: textWidth=%d textHeight=%d nrows=%d\n",textWidth,textHeight,nrows));

  // All is clean
  flags&=~FLAG_RECALC;
  }

/*******************************************************************************/

// Determine content width of scroll area
FXint FXText::getContentWidth(){
  if(flags&FLAG_RECALC) recompute();
  return marginleft+marginright+textWidth;
  }


// Determine content height of scroll area
FXint FXText::getContentHeight(){
  if(flags&FLAG_RECALC) recompute();
  return margintop+marginbottom+textHeight;
  }


// Return visible scroll-area x position
FXint FXText::getVisibleX() const {
  return barwidth;
  }


// Return visible scroll-area y position
FXint FXText::getVisibleY() const {
  return 0;
  }


// Return visible scroll-area width
FXint FXText::getVisibleWidth() const {
  return width-vertical->getWidth()-barwidth;
  }


// Return visible scroll-area height
FXint FXText::getVisibleHeight() const {
  return height-horizontal->getHeight();
  }


// Get default width
FXint FXText::getDefaultWidth(){
  return 0<vcols ? marginleft+marginright+vcols*font->getTextWidth("8",1)+barwidth : FXScrollArea::getDefaultWidth()+barwidth;
  }


// Get default height
FXint FXText::getDefaultHeight(){
  return 0<vrows ? margintop+marginbottom+vrows*font->getFontHeight() : FXScrollArea::getDefaultHeight();
  }

/*******************************************************************************/

// Recalculate layout
void FXText::layout(){
  FXint fh=font->getFontHeight();
  FXint fw=font->getFontWidth();
  FXint oww=wrapwidth;
  FXint cursorstartpos;
  FXint anchorstartpos;

  // Compute new wrap width, which is either based on the wrap columns or on the
  // width of the window.  If a vertical scroll bar MAY be visible, assume it IS
  // so we don't get sudden surprises.
  // For mono-spaced fonts, wrapwidth is a integral multiple of font width.
  if(options&TEXT_FIXEDWRAP){
    wrapwidth=wrapcolumns*font->getTextWidth("x",1);
    }
  else{
    wrapwidth=width-barwidth-marginleft-marginright;
    if(!(options&VSCROLLER_NEVER)) wrapwidth-=vertical->getDefaultWidth();
    if(font->isFontMono()) wrapwidth=fw*(wrapwidth/fw);
    }

  // If we're wrapping, and wrap width changed, we may need to reflow the text.
  if((options&TEXT_WORDWRAP) && (wrapwidth!=oww)) flags|=FLAG_RECALC;

  // Adjust scrollbars; if necessary, remeasure reflowed text
  // This places the scrollbars, and thus sets the visible area.
  placeScrollBars(width-barwidth,height);

  // Number of visible lines depends on viewport height
  nvisrows=(getVisibleHeight()-margintop-marginbottom+fh+fh-1)/fh;
  if(nvisrows<1) nvisrows=1;

  // Resize line start array; the plus 1 is to keep track of the start
  // of the next line just beyond the last visible one; this ensures
  // we know how long the last visible line is.
  resizeElms(visrows,nvisrows+1);

  // Recompute line start array
  calcVisRows(0,nvisrows);

  // Scroll bar line/column sizes are based on font; set these now
  vertical->setLine(fh);
  horizontal->setLine(fw);

  // Hopefully, all is still in range
  FXASSERT(0<=toprow && toprow<=nrows);
  FXASSERT(0<=toppos && toppos<=length);

  // Update cursor location parameters
  cursorstartpos=rowStart(cursorpos);
  cursorrow=rowFromPos(cursorstartpos);
  cursorcol=columnFromPos(cursorstartpos,cursorpos);
  cursorvcol=cursorcol;

  // Update anchor location parameters
  anchorstartpos=rowStart(anchorpos);
  anchorrow=rowFromPos(anchorstartpos);
  anchorcol=columnFromPos(anchorstartpos,anchorpos);
  anchorvcol=anchorcol;

  // Force repaint
  update();

  // Done
  flags&=~FLAG_DIRTY;
  }


// Propagate size change
void FXText::recalc(){
  FXScrollArea::recalc();
  flags|=FLAG_RECALC;
  }


/*******************************************************************************/

// Count characters
static FXint countchars(const FXchar* beg,const FXchar* end,FXchar ch){
  FXint result=0;
  while(beg<end){
    result+=(*beg++==ch);
    }
  return result;
  }


// Maximum number of columns in a string
static FXint maxcolumns(const FXchar* beg,const FXchar* end,FXint tabcols){
  FXint result=0,cols=0; FXuchar c;
  while(beg<end){
    c=*beg++;
    if(c=='\n'){                                // End of the line; keep track of the longest
      result=FXMAX(result,cols);
      cols=0;
      continue;
      }
    if(c=='\t'){                                // Advance by number of tab columns
      cols+=tabcols-cols%tabcols;
      continue;
      }
    cols++;
    if(c<0xC0) continue;
    beg++;
    if(c<0xE0) continue;
    beg++;
    if(c<0xF0) continue;
    beg++;
    }
  result=FXMAX(result,cols);                    // In case of unterminated last line
  return result;
  }

/*******************************************************************************/

// Expand tabs to spaces
// Parse a character at a time, replacing tabs with the equivalent amount of spaces.
static FXString detab(const FXString& text,FXint tabcols=8){
  FXString result('\0',text.length()+text.contains('\t')*tabcols);
  FXint is=0;
  FXint d=0;
  FXint s=0;
  FXuchar c;
  while(s<text.length()){
    c=text[s++];
    if(c==' '){
      result[d++]=c;
      is++;
      continue;
      }
    if(c=='\t'){
      do{
        result[d++]=' ';
        }
      while(++is%tabcols);
      continue;
      }
    if(c=='\n'){
      result[d++]=c;
      is=0;
      continue;
      }
    is++;
    result[d++]=c;
    if(c<0xC0) continue;
    result[d++]=text[s++];
    if(c<0xE0) continue;
    result[d++]=text[s++];
    if(c<0xF0) continue;
    result[d++]=text[s++];
    }
  FXASSERT(d<=result.length());
  result.trunc(d);
  return result;
  }


// Compress spaces to tabs
// Parse a character at a time, replacing runs of more than 2 spaces with tabs.
static FXString entab(const FXString& text,FXint tabcols=8){
  FXString result('\0',text.length());
  FXint is=0;
  FXint ie=0;
  FXint d=0;
  FXint s=0;
  FXint ts;
  FXuchar c;
  while(s<text.length()){
    result[d++]=c=text[s++];
    if(c==' '){                                 // Accumulate spaces
      ie+=1;
      if((ie-is)<3) continue;                   // Run of less than 3
      ts=is+tabcols-is%tabcols;
      if(ie<ts) continue;                       // Not crossing tabstop
      d-=(ie-is);                               // Back up to first space of run
      result[d++]='\t';                         // Write a tab there
      is=ts;                                    // Advance to tabstop
      d+=(ie-is);                               // Skip over spaces
      continue;
      }
    if(c=='\t'){                                // Keep the tab
      d-=(ie-is+1);                             // Back up to first space of the run
      result[d++]='\t';                         // Replace by tab
      ie+=tabcols-ie%tabcols;                   // New tab-column
      is+=tabcols-is%tabcols;
      if(is==ie) continue;                      // Reached tabstop
      result[d++]='\t';
      is+=tabcols-is%tabcols;
      continue;
      }
    if(c=='\n'){                                // Reset columns
      is=0;
      ie=0;
      continue;
      }
    is=++ie;                                    // One UTF8 character
    if(c<0xC0) continue;
    result[d++]=text[s++];
    if(c<0xE0) continue;
    result[d++]=text[s++];
    if(c<0xF0) continue;
    result[d++]=text[s++];
    }
  FXASSERT(d<=result.length());
  result.trunc(d);
  return result;
  }


// Retabbify line
// Assume original starting column of the string is indent, and the output
// starting column is outdent; this affects accounting of the tab-stops in the
// input string, and of the output string relative to the first character.
// Along the way, extra columns may be inserted or removed as per shift.
// If shift=0, indent=0, and outdent=0, this routine has the effect of harmonizing
// the output of white space according to the current tab setting ("clean indent").
// For now, we assume all unicode characters to be one column.
static FXString tabbify(const FXString& text,FXint tabcols=8,FXint indent=0,FXint outdent=0,FXint shift=0,FXbool tabs=false){
  FXString result;
  FXint oec=outdent+shift;
  FXint osc=outdent;
  FXint isc=indent;
  FXint iec=indent;
  FXint s=0;
  FXint d=0;
  FXint ntabs;
  FXuchar c;
  while(s<text.length()){
    c=text[s++];
    if(c==' '){ iec++; continue; }                              // Space is one column
    if(c=='\t'){ iec+=tabcols-iec%tabcols; continue; }          // Tabs is multiple columns
    oec+=(iec-isc);
    if(osc<oec){                                                // Owe some spaces
      if(tabs && 2<(oec-osc)){
        ntabs=oec/tabcols-osc/tabcols;                          // How many tabs to emit
        if(ntabs){ d+=ntabs; osc=(oec/tabcols)*tabcols; }
        }
      d+=oec-osc;
      osc=oec;
      }
    if(c=='\n'){                                                // Emit a newline and reset columns
      d++;
      isc=indent;
      iec=indent;
      osc=outdent;
      oec=outdent+shift;
      continue;
      }
    isc=++iec;                                                  // Advance input columns
    osc=++oec;                                                  // Advance output columns
    d++;                                                        // Copy character
    if(c<0xC0) continue;
    d++; 
    s++;
    if(c<0xE0) continue;
    d++;
    s++;
    if(c<0xF0) continue;
    d++; 
    s++;
    }
  result.length(d);
  oec=outdent+shift;
  osc=outdent;
  isc=indent;
  iec=indent;
  s=0;
  d=0;
  while(s<text.length()){
    c=text[s++];
    if(c==' '){ iec++; continue; }                              // Space is one column
    if(c=='\t'){ iec+=tabcols-iec%tabcols; continue; }          // Tabs is multiple columns
    oec+=(iec-isc);
    if(osc<oec){                                                // Owe some spaces
      if(tabs && 2<(oec-osc)){
        ntabs=oec/tabcols-osc/tabcols;                          // How many tabs to emit
        if(ntabs){
          do{ result[d++]='\t'; }while(--ntabs);
          osc=(oec/tabcols)*tabcols;                            // Advance starting column to the last tabstop
          }
        }
      while(osc<oec){ result[d++]=' '; osc++; }                 // Emit spaces to reach current column
      }
    if(c=='\n'){                                                // Emit a newline and reset columns
      result[d++]='\n';
      isc=indent;
      iec=indent;
      osc=outdent;
      oec=outdent+shift;
      continue;
      }
    isc=++iec;                                                  // Advance input columns
    osc=++oec;                                                  // Advance output columns
    result[d++]=c;                                              // Copy character
    if(c<0xC0) continue;
    result[d++]=text[s++];
    if(c<0xE0) continue;
    result[d++]=text[s++];
    if(c<0xF0) continue;
    result[d++]=text[s++];
    }
  FXASSERT(d<=result.length());
  result.trunc(d);
  return result;
  }

/*******************************************************************************/


// Expand tabs to spaces
// Parse a character at a time, replacing tabs with the equivalent amount of spaces.
static FXchar *detab(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,FXint tabcols=8){
  FXint ie=0; FXuchar c;
  while(src<srcend && dst<dstend){
    *dst++=c=*src++;
    if(c=='\t'){                                // Generate spaces till tab-column
      dst--;
      do{*dst++=' ';}while(++ie%tabcols);
      continue;
      }
    if(c=='\n'){                                // Reset columns
      ie=0;
      continue;
      }
    ie++;                                       // One UTF8 character
    if(c<0xC0) continue;
    *dst++=*src++;
    if(c<0xE0) continue;
    *dst++=*src++;
    if(c<0xF0) continue;
    *dst++=*src++;
    }
  FXASSERT(dst<=dstend);
  FXASSERT(src<=srcend);
  return dst;
  }


// Compress spaces to tabs
// Parse a character at a time, replacing runs of more than 2 spaces with tabs.
static FXchar* entab(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,FXint tabcols=8){
  FXint is=0,ie=0,ts; FXuchar c;
  while(src<srcend && dst<dstend){
    *dst++=c=*src++;
    if(c==' '){                                 // Accumulate spaces
      ie+=1;
      if((ie-is)<3) continue;                   // Run of less than 3
      ts=is+tabcols-is%tabcols;
      if(ie<ts) continue;                       // Not crossing tabstop
      dst-=(ie-is);                             // Back up to first space of run
      *dst++='\t';                              // Write a tab there
      is=ts;                                    // Advance to tabstop
      dst+=(ie-is);                             // Skip over spaces
      continue;
      }
    if(c=='\t'){                                // Keep the tab
      dst-=(ie-is+1);                           // Back up to first space of the run
      *dst++='\t';                              // Replace by tab
      ie+=tabcols-ie%tabcols;                   // New tab-column
      is+=tabcols-is%tabcols;
      if(is==ie) continue;                      // Reached tabstop
      *dst++='\t';
      is+=tabcols-is%tabcols;
      continue;
      }
    if(c=='\n'){                                // Reset columns
      is=ie=0;
      continue;
      }
    is=++ie;                                    // One UTF8 character
    if(c<0xC0) continue;
    *dst++=*src++;
    if(c<0xE0) continue;
    *dst++=*src++;
    if(c<0xF0) continue;
    *dst++=*src++;
    }
  FXASSERT(dst<=dstend);
  FXASSERT(src<=srcend);
  return dst;
  }

/*******************************************************************************/

// Copy columns up from col to endcol
static FXint copycols(FXchar*& dst,FXchar* dstend,const FXchar*& src,const FXchar* srcend,FXint ncols=2147483647){
  FXint col=0;
  FXuchar c;
  while(col<ncols && src<srcend && (c=*src)!='\n' && dst<dstend){
    *dst++=*src++; col++;
    if(c<0xC0) continue;
    *dst++=*src++;
    if(c<0xE0) continue;
    *dst++=*src++;
    if(c<0xF0) continue;
    *dst++=*src++;
    }
  FXASSERT(src<=srcend);
  FXASSERT(dst<=dstend);
  return col;
  }


// Skip columns from col to endcol
static FXint skipcols(const FXchar*& src,const FXchar* srcend,FXint ncols=2147483647){
  FXint col=0;
  FXuchar c;
  while(col<ncols && src<srcend && (c=*src)!='\n'){
    src++; col++;
    if(c<0xC0) continue;
    src++;
    if(c<0xE0) continue;
    src++;
    if(c<0xF0) continue;
    src++;
    }
  return col;
  }


// Padd output until endcol
static FXint padcols(FXchar*& dst,FXchar* dstend,FXint ncols=0){
  FXint col=0;
  while(col<ncols && dst<dstend){
    *dst++=' '; col++;
    }
  return col;
  }

/*******************************************************************************/

// Remove columns startcol up to endcol from src; assume input has been detabbed.
// For each line, copy up to startcol; then skip characters up to endcol,
// and copy the remainder of the line, up to and including newline, if any.
static FXchar* removecolumns(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,FXint startcol,FXint endcol){
  while(src<srcend && dst<dstend){
    copycols(dst,dstend,src,srcend,startcol);                   // Copy up to startcol
    skipcols(src,srcend,endcol-startcol);                       // Skip to endcol
    copycols(dst,dstend,src,srcend);                            // Copy to line end
    if(src<srcend && *src=='\n' && dst<dstend){                 // Copy newline
      *dst++=*src++;
      }
    }
  FXASSERT(src<=srcend);
  FXASSERT(dst<=dstend);
  return dst;
  }


// Remove columns startcol to endcol from lines starting at startpos to endpos
FXint FXText::removeTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,FXbool notify){
  if(startpos<=endpos && startcol<=endcol){
    FXString src,org,rep; FXchar *orgend,*repend; FXint nsrctabs;
    extractText(src,startpos,endpos-startpos);
    nsrctabs=countchars(&src[0],&src[src.length()],'\t');
    org.length(endpos-startpos+nsrctabs*tabcolumns);
    orgend=detab(&org[0],&org[org.length()],&src[0],&src[src.length()],tabcolumns);
    rep.length(orgend-&org[0]);
    repend=removecolumns(&rep[0],&rep[rep.length()],&org[0],orgend,startcol,endcol);
    if(!(options&TEXT_NO_TABS)){
      repend=entab(&rep[0],repend,&rep[0],repend,tabcolumns);
      }
    return replaceStyledText(startpos,endpos-startpos,&rep[0],repend-&rep[0],0,notify);
    }
  return 0;
  }

/*******************************************************************************/

// Extract block of columns of text from input; assume input has been detabbed.
// For each line, scan to startcol, then copy characters up to endcol to the
// destination. If there are fewer than startcol columns on the line, just
// copy a newline to indicate an empty column on that particular line.
static FXchar* extractcolumns(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,FXint startcol,FXint endcol){
  while(src<srcend && dst<dstend){
    skipcols(src,srcend,startcol);                              // Skip to startcol
    copycols(dst,dstend,src,srcend,endcol-startcol);            // Copy up to endcol
    skipcols(src,srcend);                                       // Skip to line end
    if(src<srcend && *src=='\n' && dst<dstend){                 // Copy newline
      *dst++=*src++;
      }
    }
  FXASSERT(src<=srcend);
  FXASSERT(dst<=dstend);
  return dst;
  }

// Extract block of columns
// Extract the affected text, and copy the selected columns.
void FXText::extractTextBlock(FXString& text,FXint startpos,FXint endpos,FXint startcol,FXint endcol) const {
  if(startpos<endpos && startcol<=endcol){
    FXString src; FXchar *textend; FXint ntabs;
    extractText(src,startpos,endpos-startpos);
    ntabs=countchars(&src[0],&src[endpos-startpos],'\t');
    text.length(endpos-startpos+ntabs*tabcolumns);
    textend=detab(&text[0],&text[text.length()],&src[0],&src[endpos-startpos],tabcolumns);
    textend=extractcolumns(&text[0],textend,&text[0],textend,startcol,endcol);
    if(!(options&TEXT_NO_TABS)){
      textend=entab(&text[0],textend,&text[0],textend,tabcolumns);
      }
    text.trunc(textend-&text[0]);
    }
  else{
    text.clear();
    }
  }


// Extract block of columns
FXString FXText::extractTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol) const {
  FXString text;
  extractTextBlock(text,startpos,endpos,startcol,endcol);
  return text;
  }

/*******************************************************************************/

// Replace block of columns of text with new ones; assume both source text and inserted text has been detabbed.
// Copies up to inscols of new text into the destination column
static FXchar* replacecolumns(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,const FXchar *ins,const FXchar* insend,FXint startcol,FXint endcol,FXint inscols){
  FXint sc,c;
  while(dst<dstend && (src<srcend || ins<insend)){
    sc=copycols(dst,dstend,src,srcend,startcol);                // Copy to startcol
    skipcols(src,srcend,endcol-startcol);                       // Skip to endcol
    if(ins<insend && *ins!='\n'){                               // Inserted block non-empty
      sc+=padcols(dst,dstend,startcol-sc);                      // Pad up to startcol
      sc+=copycols(dst,dstend,ins,insend,inscols);              // Copy inserted block, up to inscols
      }
    if(src<srcend && *src!='\n'){                               // Stuff past endcol
      padcols(dst,dstend,startcol+inscols-sc);                  // Pad to startcol+ninscols
      copycols(dst,dstend,src,srcend);                          // Copy the rest
      }
    c=0;
    if(ins<insend && *ins=='\n' && dst<dstend){                 // Advance over line end
      *dst=*ins++; c=1;
      }
    if(src<srcend && *src=='\n' && dst<dstend){
      *dst=*src++; c=1;
      }
    dst+=c;
    }
  FXASSERT(src<=srcend);
  FXASSERT(ins<=insend);
  FXASSERT(dst<=dstend);
  return dst;
  }

// Replace block of columns with text
// Calculating the size of the scratch array to assemble the replacing text is a bit
// complicated; it is best understood graphically:
//
//             startpos       startcol
//             |              |
//             |              | endcol
//             |              | |
//             V              v v
// startpos--->X--------------+-+----+-------------+  ^          ^
//             |              |      |             |  |          |
//             | A       A'   | R R' |  B          |  |norgrows  |
//             |              |      |             |  |          |
//             +--------------+      +-------------+  v          |
// endpos------X              |      |                           |ninsrows
//             |              |      |                           |
//             | C            |      |                           |
//             |              |      |                           |
//             |              |      |                           |
//             +--------------+-+----+                           v
//
//                            <------>
//                            ninscols
//
// Here A, B are the parts of the original text, A being the part before the selected
// block and B the part after (or inside) the selected block.  R is the newly added
// text, which may be more or fewer lines than the selected block.  C is any additional
// lines added in case the newly added text includes more lines than the selection.
// Note that A, B, and R may have lines of varying lengths [some lines may have no
// part in section B, for example].
//
// The total amount of allocated space should account for:
//
//   1) Original text (A + B), plus possibly expanded tabs,
//   2) Inserted text (R), plus possibly expanded tabs,
//   3) Extra padding (A') after some lines in (A), up to startcol,
//   4) Padding of empty lines (C), if any, up to startcol,
//   5) Padding of (R) up to startcol+ninscols.
//   6) The block being removed
//
// Some lines in A,B, and R are longer than others. Rather than calculating the exact
// amount of padding needed, its simpler just to over-estimate in a way which is guaranteed
// to be enough; this is done by just addding the whole rectangle; so we just add an extra
// (startcol+ninscols)*max(ninsrows,norgrows) as total padding for A,C, and R.
//
FXint FXText::replaceStyledTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXchar *text,FXint n,FXint style,FXbool notify){
  if(startpos<=endpos && startcol<=endcol){
    FXString org,rep,ins;
    FXchar *insend,*orgend,*repend;
    FXint norgrows,norgtabs,ninsrows,ninstabs,ninscols;
    extractText(rep,startpos,endpos-startpos);
    norgrows=countchars(&rep[0],&rep[rep.length()],'\n')+1;
    norgtabs=countchars(&rep[0],&rep[rep.length()],'\t');
    ninsrows=countchars(text,text+n,'\n')+1;
    ninstabs=countchars(text,text+n,'\t');
    ninscols=maxcolumns(text,text+n,tabcolumns);
    ins.length(n+ninstabs*tabcolumns);
    org.length(endpos-startpos+norgtabs*tabcolumns);
    insend=detab(&ins[0],&ins[ins.length()],text,text+n,tabcolumns);
    orgend=detab(&org[0],&org[org.length()],&rep[0],&rep[rep.length()],tabcolumns);

//    (orgend-&org[0]) + (startcol+ninscols+1)*Math::imax(ninsrows,norgrows) + (insend-&ins[0])

    rep.length(endpos-startpos+n+(norgtabs+ninstabs)*tabcolumns+(startcol+ninscols+1)*FXMAX(ninsrows,norgrows));
    repend=replacecolumns(&rep[0],&rep[rep.length()],&org[0],orgend,&ins[0],insend,startcol,endcol,ninscols);
    if(!(options&TEXT_NO_TABS)){
      repend=entab(&rep[0],repend,&rep[0],repend,tabcolumns);
      }
    return replaceStyledText(startpos,endpos-startpos,&rep[0],repend-&rep[0],style,notify);
    }
  return 0;
  }


// Replace block of columns with text
FXint FXText::replaceStyledTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXString& text,FXint style,FXbool notify){
  return replaceStyledTextBlock(startpos,endpos,startcol,endcol,text.text(),text.length(),style,notify);
  }


// Replace block of columns with text
FXint FXText::replaceTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXchar *text,FXint n,FXbool notify){
  return replaceStyledTextBlock(startpos,endpos,startcol,endcol,text,n,0,notify);
  }


// Replace block of columns with text
FXint FXText::replaceTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXString& text,FXbool notify){
  return replaceStyledTextBlock(startpos,endpos,startcol,endcol,text.text(),text.length(),0,notify);
  }

/*******************************************************************************/

// Overstrike columns starting at startcol with new text; assume inputs have been detabbed.
static FXchar* overstrikecolumns(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,const FXchar* ovr,const FXchar* ovrend,FXint startcol){
  FXint sc,ec; FXuchar c;
  while(src<srcend && dst<dstend){
    sc=ec=copycols(dst,dstend,src,srcend,startcol);     // Copy up to startcol
    if(ovr<ovrend && *ovr!='\n'){                       // Overstrike block is non-empty
      ec+=padcols(dst,dstend,startcol-ec);              // Pad up to column where overstrike starts
      ec+=copycols(dst,dstend,ovr,ovrend);              // Copy new overstruck block
      }
    if(src<srcend && *src!='\n'){                       // More stuff past startcol
      sc+=skipcols(src,srcend,ec-sc);                   // Skip past overstruck text
      copycols(dst,dstend,src,srcend);                  // Copy the rest
      }
    c=0;
    if(src<srcend && *src=='\n' && dst<dstend){         // Advance over line end
      *dst=*src++; c=1;
      }
    if(ovr<ovrend && *ovr=='\n' && dst<dstend){
      *dst=*ovr++; c=1;
      }
    dst+=c;
    }
  FXASSERT(src<=srcend);
  FXASSERT(ovr<=ovrend);
  FXASSERT(dst<=dstend);
  return dst;
  }

// Overstrike styled text block
FXint FXText::overstrikeStyledTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXchar *text,FXint n,FXint style,FXbool notify){
  if(startpos<=endpos && startcol<=endcol){
    FXString org,rep,ovr;
    FXchar *ovrend,*orgend,*repend;
    FXint norgrows,norgtabs,novrrows,novrtabs,novrcols;

    extractText(rep,startpos,endpos-startpos);

    norgrows=countchars(&rep[0],&rep[rep.length()],'\n')+1;
    norgtabs=countchars(&rep[0],&rep[rep.length()],'\t');

    novrrows=countchars(text,text+n,'\n')+1;
    novrtabs=countchars(text,text+n,'\t');
    novrcols=maxcolumns(text,text+n,tabcolumns);

    ovr.length(n+novrtabs*tabcolumns);
    org.length(endpos-startpos+norgtabs*tabcolumns);

    ovrend=detab(&ovr[0],&ovr[ovr.length()],text,text+n,tabcolumns);
    orgend=detab(&org[0],&org[org.length()],&rep[0],&rep[rep.length()],tabcolumns);

//    Math::imax((orgend-&org[0]),(startcol+novrcols+1)*Math::imax(novrrows,norgrows)+(ovrend-&ovr[0]))

    // Estimate sux
    rep.length(endpos-startpos+n+(norgtabs+novrtabs)*tabcolumns+(startcol+novrcols+1)*FXMAX(novrrows,norgrows));

    repend=overstrikecolumns(&rep[0],&rep[rep.length()],&org[0],orgend,&ovr[0],ovrend,startcol);

    if(!(options&TEXT_NO_TABS)){
      repend=entab(&rep[0],repend,&rep[0],repend,tabcolumns);
      }
    return replaceStyledText(startpos,endpos-startpos,&rep[0],repend-&rep[0],style,notify);
    }
  return 0;
  }


// Overstrike styled text block
FXint FXText::overstrikeStyledTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXString& text,FXint style,FXbool notify){
  return overstrikeStyledTextBlock(startpos,endpos,startcol,endcol,text.text(),text.length(),style,notify);
  }


// Overstrike text block
FXint FXText::overstrikeTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXchar *text,FXint n,FXbool notify){
  return overstrikeStyledTextBlock(startpos,endpos,startcol,endcol,text,n,0,notify);
  }


// Overstrike text block
FXint FXText::overstrikeTextBlock(FXint startpos,FXint endpos,FXint startcol,FXint endcol,const FXString& text,FXbool notify){
  return overstrikeStyledTextBlock(startpos,endpos,startcol,endcol,text.text(),text.length(),0,notify);
  }

/*******************************************************************************/

// End of overstruck character range
FXint FXText::overstruck(FXint start,FXint end,const FXchar *text,FXint n){
  if(!memchr(text,'\n',n)){
    FXint sindent,nindent,oindent,p;
    const FXchar *ptr;
    FXwchar ch;

    // Measure indent at pos
    sindent=columnFromPos(lineStart(start),start);

    // Measure indent at end of (first line of the) new text
    for(ptr=text,nindent=sindent; ptr<text+n; ptr=wcinc(ptr)){
      nindent+=CC(*ptr,nindent);
      }

    // Now figure out how much text to replace
    for(p=start,oindent=sindent; p<length; p+=getCharLen(p)){
      ch=getChar(p);
      if(ch=='\n') break;                // Stuff past the newline just gets inserted
      oindent+=CC(ch,oindent);
      if(oindent>=nindent){              // Replace string fits inside here
        if(oindent==nindent) p+=getCharLen(p);
        break;
        }
      }
    end=p;
    }
  return end;
  }

/*******************************************************************************/

// Insert same text at given column on each line.
static FXchar* insertcolumns(FXchar* dst,FXchar* dstend,const FXchar* src,const FXchar* srcend,const FXchar *ins,const FXchar* insend,FXint startcol,FXint inscols){
  FXint sc;
  while(dst<dstend && src<srcend){
    sc=copycols(dst,dstend,src,srcend,startcol);                // Copy to startcol
    if(ins<insend && *ins!='\n'){                               // Inserted block non-empty
      sc+=padcols(dst,dstend,startcol-sc);                      // Pad up to startcol where insert starts
      sc+=copycols(dst,dstend,ins,insend,inscols);              // Insert string, up to newline
      }
    if(src<srcend && *src!='\n'){                               // Stuff past endcol
      padcols(dst,dstend,startcol+inscols-sc);                  // Pad to startcol+ninscols
      copycols(dst,dstend,src,srcend);                          // Copy the rest
      }
    if(src<srcend && *src=='\n' && dst<dstend){                 // Copy newline
      *dst++=*src++;
      }
    }
  FXASSERT(src<=srcend);
  FXASSERT(ins<=insend);
  FXASSERT(dst<=dstend);
  return dst;
  }

// Insert text of n characters at column startcol for each line from startpos...endpos
FXint FXText::insertTextBlock(FXint startpos,FXint endpos,FXint startcol,const FXchar *text,FXint n,FXbool notify){
  if(startpos<=endpos){
    FXString org,rep,ins; FXchar *orgend,*repend; FXint norgrows,norgtabs,ninstabs,ninscols;
    extractText(rep,startpos,endpos-startpos);
    norgrows=countchars(&rep[0],&rep[rep.length()],'\n')+1;
    norgtabs=countchars(&rep[0],&rep[rep.length()],'\t');
    org.length(endpos-startpos+norgtabs*tabcolumns);
    orgend=detab(&org[0],&org[org.length()],&rep[0],&rep[rep.length()],tabcolumns);
    ninstabs=countchars(text,text+n,'\t');
    ninscols=maxcolumns(text,text+n,tabcolumns);
    rep.length(orgend-&org[0]+norgrows*(n+ninstabs*tabcolumns));
    repend=insertcolumns(&rep[0],&rep[rep.length()],&org[0],orgend,text,text+n,startcol,ninscols);
    if(!(options&TEXT_NO_TABS)){
      repend=entab(&rep[0],repend,&rep[0],repend,tabcolumns);
      }
    return replaceText(startpos,endpos-startpos,&rep[0],repend-&rep[0],notify);
    }
  return 0;
  }

/*******************************************************************************/

// Shift block of lines from position start up to end by given indent
FXint FXText::shiftText(FXint startpos,FXint endpos,FXint shift,FXbool notify){
  if(startpos<endpos){
    FXString org=extractText(startpos,endpos-startpos);
    FXString rep=tabbify(org,tabcolumns,0,0,shift,!(options&TEXT_NO_TABS));
    return replaceStyledText(startpos,endpos-startpos,rep,0,notify);
    }
  return 0;
  }

/*******************************************************************************/

// Shift case of text
FXint FXText::caseShift(FXint startpos,FXint endpos,FXint upper,FXbool notify){
  if(startpos<endpos){
    FXString text;
    extractText(text,startpos,endpos-startpos);
    switch(upper){
      case 1: text.upper(); break;
      case 0: text.lower(); break;
      }
    replaceText(startpos,endpos-startpos,text,notify);
    return text.length();
    }
  return 0;
  }

/*******************************************************************************/

// Search for text
FXbool FXText::findText(const FXString& string,FXint* beg,FXint* end,FXint start,FXuint flgs,FXint npar){
  FXint rexmode=FXRex::Normal;
  FXRex rex;

  // Check arguments
  if(npar<1 || !beg || !end){ fxerror("%s::findText: bad argument.\n",getClassName()); }

  // Tweak parse flags a bit
  if(1<npar) rexmode|=FXRex::Capture;                           // Capturing parentheses
  if(flgs&SEARCH_IGNORECASE) rexmode|=FXRex::IgnoreCase;        // Case insensitivity
  if(!(flgs&SEARCH_REGEX)) rexmode|=FXRex::Verbatim;            // Verbatim match

  // Try parse the regex
  if(rex.parse(string,rexmode)==FXRex::ErrOK){

    // Make all characters contiguous in the buffer
    squeezegap();

    // Search forward
    if(flgs&SEARCH_FORWARD){
      if(start<=length){
        if(rex.search(buffer,length,FXMAX(start,0),length,FXRex::Normal,beg,end,npar)>=0) return true;
        }
      if((flgs&SEARCH_WRAP) && (start>0)){
        if(rex.search(buffer,length,0,FXMIN(start,length),FXRex::Normal,beg,end,npar)>=0) return true;
        }
      return false;
      }

    // Search backward
    if(flgs&SEARCH_BACKWARD){
      if(0<=start){
        if(rex.search(buffer,length,FXMIN(start,length),0,FXRex::Normal,beg,end,npar)>=0) return true;
        }
      if((flgs&SEARCH_WRAP) && (start<length)){
        if(rex.search(buffer,length,length,FXMAX(start,0),FXRex::Normal,beg,end,npar)>=0) return true;
        }
      return false;
      }

    // Anchored match
    return rex.amatch(buffer,length,start,FXRex::Normal,beg,end,npar);
    }
  return false;
  }

/*******************************************************************************/

// Localize position at x,y
FXint FXText::getPosAt(FXint x,FXint y) const {
  FXint linebeg,lineend,row,cx=0,cw,p;
  FXwchar c;
  x=x-pos_x-marginleft-getVisibleX();
  y=y-pos_y-margintop-getVisibleY();
  row=y/font->getFontHeight();
  if(row<toprow){                       // Above visible area
    if(row<0) return 0;                 // Before first row
    linebeg=prevRow(visrows[0],toprow-row);
    lineend=nextRow(linebeg);
    }
  else if(row>=toprow+nvisrows){        // Below visible area
    if(row>=nrows) return length;       // Below last row
    linebeg=nextRow(visrows[nvisrows-1],row-toprow-nvisrows+1);
    lineend=nextRow(linebeg);
    }
  else{                                 // Inside visible area
    linebeg=visrows[row-toprow];
    lineend=visrows[row-toprow+1];
    }
  if(linebeg<lineend){                  // Backup past line-break character, space or newline
    p=dec(lineend);
    if(Unicode::isSpace(getChar(p))) lineend=p;
    }
  FXASSERT(0<=linebeg);
  FXASSERT(linebeg<=lineend);
  FXASSERT(lineend<=length);
  while(linebeg<lineend){
    c=getChar(linebeg);
    cw=charWidth(c,cx);
    if(x<=(cx+(cw>>1))) return linebeg; // Before middle of character
    linebeg+=getCharLen(linebeg);
    cx+=cw;
    }
  return lineend;
  }


// Return text position containing x, y coordinate
FXint FXText::getPosContaining(FXint x,FXint y) const {
  FXint linebeg,lineend,row,cx=0,cw,p;
  FXwchar c;
  x=x-pos_x-marginleft-getVisibleX();
  y=y-pos_y-margintop-getVisibleY();
  row=y/font->getFontHeight();
  if(row<toprow){                       // Above visible area
    if(row<0) return 0;                 // Before first row
    linebeg=prevRow(visrows[0],toprow-row);
    lineend=nextRow(linebeg);
    }
  else if(row>=toprow+nvisrows){        // Below visible area
    if(row>=nrows) return length;       // Below last row
    linebeg=nextRow(visrows[nvisrows-1],row-toprow-nvisrows+1);
    lineend=nextRow(linebeg);
    }
  else{                                 // Inside visible area
    linebeg=visrows[row-toprow];
    lineend=visrows[row-toprow+1];
    }
  if(linebeg<lineend){                  // Backup past line-break character, space or newline
    p=dec(lineend);
    if(Unicode::isSpace(getChar(p))) lineend=p;
    }
  FXASSERT(0<=linebeg);
  FXASSERT(linebeg<=lineend);
  FXASSERT(lineend<=length);
  while(linebeg<lineend){
    c=getChar(linebeg);
    cw=charWidth(c,cx);
    if(x<cx+cw) return linebeg;         // Character contains x
    linebeg+=getCharLen(linebeg);
    cx+=cw;
    }
  return lineend;
  }


// Return closest position and (row,col) of given x,y coordinate.
// Computing the logical column inside of a tab, things can get tricky when
// the font is not a fixed-pitch.  Our solution is to stretch spaces to
// subdivide the tab into as many columns as needed, regardless of whether
// the space is a whole multiple of the regular space width.
// Also, control-characters are problematic as they're rendered as ^A,
// thus, take up two columns even for fixed-pitch fonts.
FXint FXText::getRowColumnAt(FXint x,FXint y,FXint& row,FXint& col) const {
  register FXint spacew=font->getCharWidth(' ');
  register FXint caretw=font->getCharWidth('^');
  register FXint linebeg,lineend,cx=0,cw,cc,p;
  register FXwchar c;
  x=x-pos_x-marginleft-getVisibleX();
  y=y-pos_y-margintop-getVisibleY();
  row=y/font->getFontHeight();          // Row is easy to find
  row=FXCLAMP(0,row,nrows-1);           // Keep inside the legal range
  col=0;                                // Find column later
  if(row<toprow){                       // Above visible area
    linebeg=prevRow(visrows[0],toprow-row);
    lineend=nextRow(linebeg);
    }
  else if(row>=toprow+nvisrows){        // Below visible area
    linebeg=nextRow(visrows[nvisrows-1],row-toprow-nvisrows+1);
    lineend=nextRow(linebeg);
    }
  else{                                 // Inside visible area
    linebeg=visrows[row-toprow];
    lineend=visrows[row-toprow+1];
    }
  if(linebeg<lineend){                  // Backup past line-break character, space or newline
    p=dec(lineend);
    if(Unicode::isSpace(getChar(p))) lineend=p;
    }
  FXASSERT(0<=linebeg);
  FXASSERT(linebeg<=lineend);
  FXASSERT(lineend<=length);
  while(linebeg<lineend){
    c=getChar(linebeg);
    if(' '<=c){                         // Normal character
      cw=font->getCharWidth(c);
      if((cx+(cw>>1))<x){
        linebeg+=getCharLen(linebeg);   // Advance over utf8 character
        col+=1;
        cx+=cw;
        continue;
        }
      return linebeg;
      }
    else if(c=='\t'){                   // Tab is really complex
      cw=tabwidth-cx%tabwidth;
      cc=tabcolumns-col%tabcolumns;
      if(cx+cw<=x){                     // Advance over entire tab
        linebeg+=1;
        col+=cc;
        cx+=cw;
        continue;
        }
      if(cx<x){                         // Calculate column inside tab
        col+=(cc*(x-cx)+(cw>>1))/cw;
        linebeg+=(x>=cx+(cw>>1));       // Round to nearest position
        }
      return linebeg;
      }
    else{                               // Control characters
      cw=caretw+font->getCharWidth(c|0x40);
      if((cx+(cw>>1))<x){
        linebeg+=1;
        col+=1;
        cx+=cw;
        continue;
        }
      return linebeg;
      }
    }
  if(cx<x){                             // Calculate column beyond end of line
    col+=(x+(spacew>>1)-cx)/spacew;
    }
  return linebeg;
  }


// Calculate X position of pos
FXint FXText::getXOfPos(FXint pos) const {
  register FXint base=rowStart(pos);
  return getVisibleX()+marginleft+pos_x+xoffset(base,pos);
  }


// Determine Y from position pos
FXint FXText::getYOfPos(FXint pos) const {
  register FXint h=font->getFontHeight();
  return getVisibleY()+margintop+pos_y+rowFromPos(pos)*h;
  }


// Return screen x-coordinate of row and column
FXint FXText::getXOfRowColumn(FXint row,FXint col) const {
  register FXint spacew=font->getCharWidth(' ');
  register FXint caretw=font->getCharWidth('^');
  register FXint linebeg,lineend,tcol=0,twid=0,tadj=0,cx=0,cc=0,cw,p;
  register FXwchar c;
  if(row<toprow){                       // Above visible area
    linebeg=prevRow(visrows[0],toprow-row);
    lineend=nextRow(linebeg);
    }
  else if(row>=toprow+nvisrows){        // Below visible area
    linebeg=nextRow(visrows[nvisrows-1],row-toprow-nvisrows+1);
    lineend=nextRow(linebeg);
    }
  else{                                 // Inside visible area
    linebeg=visrows[row-toprow];
    lineend=visrows[row-toprow+1];
    }
  if(linebeg<lineend){                  // Backup past line-break character, space or newline
    p=dec(lineend);
    if(Unicode::isSpace(getChar(p))) lineend=p;
    }
  FXASSERT(0<=linebeg);
  FXASSERT(linebeg<=lineend);
  FXASSERT(lineend<=length);
  while(cc<col){
    if(linebeg>=lineend){               // Column past end of line
      cx+=spacew*(col-cc);              // Add left-over columns and we're done
      break;
      }
    c=getChar(linebeg);
    if(' '<=c){                         // Normal character
      cx+=font->getCharWidth(c);
      cc+=1;
      linebeg+=getCharLen(linebeg);     // Advance over utf8 character
      continue;
      }
    if(c!='\t'){                        // Control character
      cw+=caretw+font->getCharWidth(c|0x40);
      cc+=1;
      linebeg+=1;
      continue;
      }
    if(tcol==0){                        // Tab character
      cw=tabwidth-cx%tabwidth;
      tcol=tabcolumns-cc%tabcolumns;
      twid=cw/tcol;
      tadj=cw-twid*tcol;
      }
    cx+=twid+(tadj>0);                  // Mete out bits of tab character
    tcol-=1;
    tadj-=1;
    cc+=1;
    linebeg+=(tcol==0);
    }
  return getVisibleX()+marginleft+pos_x+cx;
  }


// Return screen y-coordinate of row and column
FXint FXText::getYOfRowColumn(FXint row,FXint) const {
  return getVisibleY()+margintop+pos_y+row*font->getFontHeight();
  }

/*******************************************************************************/

// A position is selected if the character position is inside the non-empty character
// range. For a block-select, a position is selected if the character position is inside
// the selected characters, and the column is inside the range of selected columns.
FXbool FXText::isPosSelected(FXint pos,FXint col) const {
  return (select.startpos<=pos && pos<=select.endpos) && ((select.startcol>select.endcol) || (select.startcol<=col && col<=select.endcol));
  }


// See if position is in the range selection, and the selection is non-empty
FXbool FXText::isPosSelected(FXint pos) const {
  return select.startpos<=pos && pos<=select.endpos;
  }


// Return true if line containing position is fully visible
FXbool FXText::isPosVisible(FXint pos) const {
  if(visrows[0]<=pos && pos<=visrows[nvisrows]){
    FXint vy=getVisibleY();
    FXint vh=getVisibleHeight();
    FXint y=getYOfPos(pos);
    return vy+margintop<=y && y<=vy+vh-marginbottom-font->getFontHeight();
    }
  return false;
  }


// Force position to become fully visible
void FXText::makePositionVisible(FXint pos){
  FXint vx=getVisibleX();
  FXint vy=getVisibleY();
  FXint vw=getVisibleWidth();
  FXint vh=getVisibleHeight();
  FXint x=getXOfPos(pos);
  FXint y=getYOfPos(pos);
  FXint h=font->getFontHeight();
  FXint ny=pos_y;
  FXint nx=pos_x;

  // Check vertical visibility
  if(y<vy+margintop){
    ny=pos_y+vy+margintop-y;
    nx=0;
    }
  else if(y>vy+vh-marginbottom-h){
    ny=pos_y+vy+vh-marginbottom-h-y;
    nx=0;
    }

  // Check horizontal visibility
  if(x<vx+marginleft){
    nx=pos_x+vx+marginleft-x;
    }
  else if(x>vx+vw-marginright){
    nx=pos_x+vx+vw-marginright-x;
    }

  // If needed, scroll
  if(nx!=pos_x || ny!=pos_y){
    setPosition(nx,ny);
    }
  }


// Make line containing pos the top visible line
void FXText::setTopLine(FXint pos){
  setPosition(pos_x,-rowFromPos(pos)*font->getFontHeight());
  }


// Make line containing pos the bottom visible line
void FXText::setBottomLine(FXint pos){
  setPosition(pos_x,getVisibleHeight()-marginbottom-margintop-font->getFontHeight()-rowFromPos(pos)*font->getFontHeight());
  }


// Center line containing pos to center of the screen
void FXText::setCenterLine(FXint pos){
  setPosition(pos_x,((getVisibleHeight()-marginbottom-margintop)/2)-rowFromPos(pos)*font->getFontHeight());
  }


// Get top line
FXint FXText::getTopLine() const {
  return visrows[0];
  }


// Get bottom line
FXint FXText::getBottomLine() const {
  return visrows[nvisrows-1];
  }


// Move content
void FXText::moveContents(FXint x,FXint y){
  register FXint delta=-y/font->getFontHeight()-toprow;
  register FXint vx=getVisibleX();
  register FXint vy=getVisibleY();
  register FXint vw=getVisibleWidth();
  register FXint vh=getVisibleHeight();
  register FXint dx=x-pos_x;
  register FXint dy=y-pos_y;
  register FXint i;

  // Erase fragments of cursor overhanging margins
  eraseCursorOverhang();

  // Scrolled up one or more lines
  if(delta<0){
    if(toprow+delta<=0){
      toppos=0;
      toprow=0;
      }
    else{
      toppos=prevRow(toppos,-delta);
      toprow=toprow+delta;
      }
    if(-delta<nvisrows){
      for(i=nvisrows; i>=-delta; i--) visrows[i]=visrows[delta+i];
      calcVisRows(0,-delta);
      }
    else{
      calcVisRows(0,nvisrows);
      }
    }

  // Scrolled down one or more lines
  else if(delta>0){
    if(toprow+delta>=nrows-1){
      toppos=rowStart(length);
      toprow=nrows-1;
      }
    else{
      toppos=nextRow(toppos,delta);
      toprow=toprow+delta;
      }
    if(delta<nvisrows){
      for(i=0; i<=nvisrows-delta; i++) visrows[i]=visrows[delta+i];
      calcVisRows(nvisrows-delta,nvisrows);
      }
    else{
      calcVisRows(0,nvisrows);
      }
    }

  // This is now the new keep position
  keeppos=toppos;

  // Hopefully, all is still in range
  FXASSERT(0<=toprow && toprow<nrows);
  FXASSERT(0<=toppos && toppos<=length);

  // Scroll stuff in the bar only vertically
  scroll(0,vy+margintop,vx,vh-margintop-marginbottom,0,dy);

  // Scroll the text
  scroll(vx+marginleft,vy+margintop,vw-marginleft-marginright,vh-margintop-marginbottom,dx,dy);

  pos_x=x;
  pos_y=y;
  }

/*******************************************************************************/

// Move the cursor
void FXText::setCursorPos(FXint pos,FXbool notify){
  pos=validPos(pos);
  if(cursorpos!=pos){
    if(isEditable()) drawCursor(0);
    if(options&TEXT_SHOWACTIVE){ updateRow(cursorrow); }
    FXint cursorstartpos=rowStart(pos);
    cursorrow=rowFromPos(cursorstartpos);
    cursorcol=columnFromPos(cursorstartpos,pos);
    cursorvcol=cursorcol;
    cursorpos=pos;
    prefcol=-1;
    if(options&TEXT_SHOWACTIVE){ updateRow(cursorrow); }
    if(isEditable()) drawCursor(FLAG_CARET);
    if(target && notify){
      target->tryHandle(this,FXSEL(SEL_CHANGED,message),(void*)(FXival)cursorpos);
      }
    }
  blink=FLAG_CARET;
  }


// Set cursor row, column
void FXText::setCursorRowColumn(FXint row,FXint col,FXbool notify){
  row=FXCLAMP(0,row,nrows-1);
  col=FXMAX(col,0);
  if((row!=cursorrow) || (col!=cursorvcol)){
    FXint newstart=posFromRow(row);             // Row start of new row
    FXint newpos=posFromColumn(newstart,col);   // Position of column on that row
    setCursorPos(newpos,notify);
    cursorvcol=col;
    }
  }


// Set cursor row
void FXText::setCursorRow(FXint row,FXbool notify){
  setCursorRowColumn(row,(0<=prefcol)?prefcol:cursorcol,notify);
  }


// Set cursor column
void FXText::setCursorColumn(FXint col,FXbool notify){
  setCursorRowColumn(cursorrow,col,notify);
  }


// Move cursor
void FXText::moveCursor(FXint pos,FXbool notify){
  setCursorPos(pos,notify);
  setAnchorPos(pos);
  makePositionVisible(cursorpos);
  killSelection(notify);
  flashMatching();
  }


// Move cursor to row and column, and scroll into view
void FXText::moveCursorRowColumn(FXint row,FXint col,FXbool notify){
  setCursorRowColumn(row,col,notify);
  setAnchorRowColumn(row,col);
  makePositionVisible(cursorpos);
  killSelection(notify);
  flashMatching();
  }


// Move cursor and select
void FXText::moveCursorAndSelect(FXint pos,FXuint sel,FXbool notify){
  killHighlight();
  setCursorPos(pos,notify);
  makePositionVisible(cursorpos);
  extendSelection(cursorpos,sel,notify);
  }


// Move cursor to row and column, and extend the block selection to this point
void FXText::moveCursorRowColumnAndSelect(FXint row,FXint col,FXbool notify){
  killHighlight();
  setCursorRowColumn(row,col,notify);
  makePositionVisible(cursorpos);
  extendBlockSelection(row,col,notify);
  }


// Set anchor position
void FXText::setAnchorPos(FXint pos){
  pos=validPos(pos);
  if(anchorpos!=pos){
    FXint anchorstartpos=rowStart(pos);
    anchorrow=rowFromPos(anchorstartpos);
    anchorcol=columnFromPos(anchorstartpos,pos);
    anchorpos=pos;
    anchorvcol=anchorcol;
    }
  }


// Set anchor row and column
void FXText::setAnchorRowColumn(FXint row,FXint col){
  row=FXCLAMP(0,row,nrows-1);
  col=FXMAX(col,0);
  if((row!=anchorrow) || (col!=anchorvcol)){
    FXint newstart=posFromRow(row);             // Row start of new row
    FXint newpos=posFromColumn(newstart,col);   // Position of column on that row
    setAnchorPos(newpos);
    anchorvcol=col;
    }
  }

/*******************************************************************************/

// At position pos, ncdel old characters have been replaced by ncins new ones,
// and nrdel old rows have been replaced with nrins new rows. Recalculate the
// visrows[] array and ancillary buffer positioning information.
void FXText::mutation(FXint pos,FXint ncins,FXint ncdel,FXint nrins,FXint nrdel){
  register FXint th=font->getFontHeight();
  register FXint vx=getVisibleX();
  register FXint vy=getVisibleY();
  register FXint vw=getVisibleWidth();
  register FXint vh=getVisibleHeight();
  register FXint ncdelta=ncins-ncdel;
  register FXint nrdelta=nrins-nrdel;
  register FXint line,i,y;

  FXTRACE((150,"BEFORE: pos=%d ncins=%d ncdel=%d nrins=%d nrdel=%d toppos=%d toprow=%d nrows=%d nvisrows=%d length=%d\n",pos,ncins,ncdel,nrins,nrdel,toppos,toprow,nrows,nvisrows,length));

  // Changes below top of buffer
  if(visrows[0]<=pos){

    // Changes in bottom part of visible buffer
    if(pos<=visrows[nvisrows]){

      // Line is in visible part of buffer
      line=rowFromPos(pos)-toprow;
      FXASSERT(0<=line && line<nvisrows);

      // More lines
      if(nrdelta>0){
        for(i=nvisrows; i>=line+nrins; i--) visrows[i]=visrows[i-nrdelta]+ncdelta;
        calcVisRows(line,line+nrins);
        y=vy+pos_y+margintop+(toprow+line)*th;
        update(vx,y,vw,vh-y);                   // Repaint bottom part
        FXASSERT(0<=visrows[0]);
        FXASSERT(visrows[nvisrows]<=length);
        }

      // Fewer lines
      else if(nrdelta<0){
        for(i=line+nrdel; i<=nvisrows; i++) visrows[i+nrdelta]=visrows[i]+ncdelta;
        calcVisRows(line,line+nrins);
        calcVisRows(nvisrows+nrdelta,nvisrows);
        y=vy+pos_y+margintop+(toprow+line)*th;
        update(vx,y,vw,vh-y);                   // Repaint bottom part
        FXASSERT(0<=visrows[0]);
        FXASSERT(visrows[nvisrows]<=length);
        }

      // Same lines
      else{
        for(i=line+nrdel; i<=nvisrows; i++) visrows[i]=visrows[i]+ncdelta;
        calcVisRows(line,line+nrins);
        if(nrins==0){
          y=vy+pos_y+margintop+(toprow+line)*th;
          update(vx,y,vw,th);                  // Repaint one line
          }
        else{
          y=vy+pos_y+margintop+(toprow+line)*th;
          update(vx,y,vw,nrins*th);             // Repaint nrins lines
          }
        FXASSERT(0<=visrows[0]);
        FXASSERT(visrows[nvisrows]<=length);
        }
      }
    }

  // Changes above bottom of buffer
  else if(pos+ncdel<visrows[nvisrows]){

    // Changes in top visible part of buffer
    if(visrows[0]<pos+ncdel){

      // Line is in visible part of buffer
      line=rowFromPos(pos+ncdel)-toprow;
      FXASSERT(0<=line && line<nvisrows);

      // Enough text to keep bottom part of buffer
      if(line<=toprow+nrdelta){
        toprow+=nrdelta;
        toppos=prevRow(visrows[line]+ncdelta,line);
        keeppos=toppos;
        FXASSERT(0<=toprow);
        FXASSERT(nextRow(0,toprow)==toppos);
        pos_y-=nrdelta*th;
        for(i=line; i<=nvisrows; i++) visrows[i]=visrows[i]+ncdelta;
        calcVisRows(0,line);
        update(vx,vy,vw,pos_y+margintop+(toprow+line)*th);
        if(nrdelta) update(0,vy,vx,vh);         // Repaint line numbers
        }

      // Not enough text in buffer to avoid scrolling
      else{
        toprow=0;
        toppos=0;
        keeppos=0;
        pos_y=0;
        calcVisRows(0,nvisrows);
        update();                               // Repaint all
        }
      }

    // Changes above visible part of buffer
    else{
      toprow+=nrdelta;
      toppos+=ncdelta;
      keeppos=toppos;
      FXASSERT(0<=toprow);
      FXASSERT(nextRow(0,toprow)==toppos);
      for(i=0; i<=nvisrows; i++) visrows[i]+=ncdelta;
      FXASSERT(0<=visrows[0]);
      FXASSERT(visrows[nvisrows]<=length);
      pos_y-=nrdelta*th;
      if(nrdelta) update(0,vy,vx,vh);           // Repaint only line numbers
      }
    }

  // Changes affect all of visible buffer
  else{
    toprow=FXMAX(0,FXMIN(toprow,nrows-nvisrows));
    toppos=nextRow(0,toprow);
    keeppos=toppos;
    pos_y=-toprow*th;
    calcVisRows(0,nvisrows);
    update();                                   // Repaint all
    }
  FXTRACE((150,"AFTER : pos=%d ncins=%d ncdel=%d nrins=%d nrdel=%d toppos=%d toprow=%d nrows=%d nvisrows=%d length=%d\n",pos,ncins,ncdel,nrins,nrdel,toppos,toprow,nrows,nvisrows,length));
  }



// Adjust selection for change in text, if there is a selection
static void adjustSelection(FXTextSelection& sel,FXint pos,FXint ndel,FXint nins){
//#define SELECTION_SNIPPED 1
  if(sel.startpos<=sel.endpos){
    if(pos+ndel<=sel.startpos){         // No overlap with change, just adjust positions
      sel.startpos+=nins-ndel;
      sel.endpos+=nins-ndel;
      }
    else if(pos<=sel.startpos){
      if(pos+ndel<=sel.endpos){         // First part of selection inside change
        sel.endpos+=nins-ndel;
#ifdef SELECTION_SNIPPED
        sel.startpos=pos+nins;
#else
        sel.startpos=pos;
#endif
        }
      else{                             // Whole of selection inside change
#ifdef SELECTION_SNIPPED
        sel.startpos=0;
        sel.endpos=-1;
        sel.startcol=0;
        sel.endcol=-1;
#else
        sel.startpos=pos;
        sel.endpos=pos+nins;
#endif
        }
      }
    else if(pos<sel.endpos){
      if(pos+ndel<=sel.endpos){         // Selection covers change completely
        sel.endpos+=nins-ndel;
        }
      else{                             // Last part of selection inside change
#ifdef SELECTION_SNIPPED
        sel.endpos=pos;
#else
        sel.endpos=pos+nins;
#endif
        }
      }
    }
  }


// Backs up to the begin of the line preceding the line containing pos, or the
// start of the line containing pos if the preceding line terminated in a newline.
FXint FXText::changeBeg(FXint pos) const {
  register FXint p1,p2,t;
  FXASSERT(0<=pos && pos<=length);
  p1=p2=lineStart(pos);
  if(options&TEXT_WORDWRAP){
    while(p2<pos && (t=wrap(p2))<=pos){
      p1=p2;
      p2=t;
      }
    }
  FXASSERT(0<=p1 && p1<=length);
  return p1;
  }


// Scan forward to the end of affected area, which is the start of the next
// paragraph; a change can cause the rest of the paragraph to reflow.
FXint FXText::changeEnd(FXint pos) const {
  FXASSERT(0<=pos && pos<=length);
  while(pos<length){
    if(getByte(pos)=='\n') return pos+1;
    pos++;
    }
  return length+1;      // FIXME
//  return length;      // FIXME
  }


// Replace m characters at pos by n characters
void FXText::replace(FXint pos,FXint m,const FXchar *text,FXint n,FXint style){
  FXint nrdel,nrins,ncdel,ncins,wbeg,wend,del,wdel,hdel,wins,hins,cursorstartpos,anchorstartpos;

  FXTRACE((150,"pos=%d mdel=%d nins=%d\n",pos,m,n));

  // Delta in characters
  del=n-m;

  // Bracket potentially affected character range for wrapping purposes
  wbeg=changeBeg(pos);
  wend=changeEnd(pos+m);

  // Measure stuff before change
  nrdel=measureText(wbeg,wend,wdel,hdel);
  ncdel=wend-wbeg;

  FXTRACE((150,"wbeg=%d wend=%d nrdel=%d ncdel=%d length=%d nrows=%d wdel=%d hdel=%d\n",wbeg,wend,nrdel,ncdel,length,nrows,wdel,hdel));

  // Modify the buffer
  sizegap(del);
  movegap(pos);
  copyElms(&buffer[pos],text,n);
  if(sbuffer){fillElms(&sbuffer[pos],style,n);}
  gapstart+=n;
  gapend+=m;
  length+=del;

  // Measure stuff after change
  nrins=measureText(wbeg,wend+n-m,wins,hins);
  ncins=wend+n-m-wbeg;

  // Adjust number of rows now
  nrows+=nrins-nrdel;

  FXTRACE((150,"wbeg=%d wend+n-m=%d nrins=%d ncins=%d length=%d nrows=%d wins=%d hins=%d\n",wbeg,wend+n-m,nrins,ncins,length,nrows,wins,hins));

  // Update visrows array and other stuff
  mutation(wbeg,ncins,ncdel,nrins,nrdel);

  // Fix text metrics
  textHeight=textHeight+hins-hdel;
  textWidth=FXMAX(textWidth,wins);

  // Fix selection ranges
  adjustSelection(select,pos,m,n);
  adjustSelection(hilite,pos,m,n);

  // Keep anchorpos at same place relative to its surrounding text.
  // When inside the changed region, move it to the end of the change.
  if(wend<=anchorpos){
    anchorpos+=del;                                     // Adjust position
    anchorrow+=nrins-nrdel;                             // Adjust row
    }
  else if(wbeg<=anchorpos){
    if(pos+m<=anchorpos) anchorpos+=del;                // Beyond changed text
    else if(pos<=anchorpos) anchorpos=pos+n;            // To end of changed text
    anchorstartpos=rowStart(anchorpos);
    anchorrow=rowFromPos(anchorstartpos);
    anchorcol=columnFromPos(anchorstartpos,anchorpos);
    anchorvcol=anchorcol;
    }

  // Keep cursorpos at same place relative to its surrounding text.
  // When inside the changed region, move it to the end of the change.
  if(wend<=cursorpos){
    cursorpos+=del;                                     // Adjust position
    cursorrow+=nrins-nrdel;                             // Adjust row
    }
  else if(wbeg<=cursorpos){
    if(pos+m<=cursorpos) cursorpos+=del;                // Beyond changed text
    else if(pos<=cursorpos) cursorpos=pos+n;            // To end of changed text
    cursorstartpos=rowStart(cursorpos);
    cursorrow=rowFromPos(cursorstartpos);
    cursorcol=columnFromPos(cursorstartpos,cursorpos);
    cursorvcol=cursorcol;
    }

  // Hopefully it all still makes sense
  FXASSERT(0<=anchorpos && anchorpos<=length);
  FXASSERT(0<=cursorpos && cursorpos<=length);

  // Reconcile scrollbars
  placeScrollBars(width-barwidth,height);

  // Forget preferred column
  prefcol=-1;

  // Text was changed
  modified=true;
  }

/*******************************************************************************/

// Change the text in the buffer to new text
FXint FXText::setText(const FXchar* text,FXint n,FXbool notify){
  return setStyledText(text,n,0,notify);
  }


// Change all of the text
FXint FXText::setText(const FXString& text,FXbool notify){
  return setStyledText(text,0,notify);
  }


// Change the text in the buffer to new text
FXint FXText::setStyledText(const FXchar* text,FXint n,FXint style,FXbool notify){
  FXTextChange textchange;
  if(n<0){ fxerror("%s::setStyledText: bad argument.\n",getClassName()); }
  if(!resizeElms(buffer,n+MINSIZE)){
    fxerror("%s::setStyledText: out of memory.\n",getClassName());
    }
  copyElms(buffer,text,n);
  if(sbuffer){
    if(!resizeElms(sbuffer,n+MINSIZE)){
      fxerror("%s::setStyledText: out of memory.\n",getClassName());
      }
    fillElms(sbuffer,style,n);
    }
  gapstart=n;
  gapend=gapstart+MINSIZE;
  length=n;
  toppos=0;
  toprow=0;
  keeppos=0;
  select.startpos=0;
  select.endpos=-1;
  select.startcol=0;
  select.endcol=-1;
  hilite.startpos=0;
  hilite.endpos=-1;
  hilite.startcol=0;
  hilite.endcol=-1;
  anchorpos=0;
  anchorrow=0;
  anchorcol=0;
  anchorvcol=0;
  cursorpos=0;
  cursorrow=0;
  cursorcol=0;
  cursorvcol=0;
  prefcol=-1;
  pos_x=0;
  pos_y=0;
  modified=false;
  textchange.pos=0;
  textchange.ndel=0;
  textchange.nins=n;
  textchange.ins=(FXchar*)text;
  textchange.del=(FXchar*)"";
  if(notify && target){
    target->tryHandle(this,FXSEL(SEL_INSERTED,message),(void*)&textchange);
    target->tryHandle(this,FXSEL(SEL_CHANGED,message),(void*)(FXival)cursorpos);
    }
  recalc();
  layout();
  update();
  return n;
  }


// Change all of the text
FXint FXText::setStyledText(const FXString& text,FXint style,FXbool notify){
  return setStyledText(text.text(),text.length(),style,notify);
  }


// Replace text by other text
FXint FXText::replaceText(FXint pos,FXint m,const FXchar *text,FXint n,FXbool notify){
  return replaceStyledText(pos,m,text,n,0,notify);
  }


// Replace text by other text
FXint FXText::replaceText(FXint pos,FXint m,const FXString& text,FXbool notify){
  return replaceStyledText(pos,m,text.text(),text.length(),0,notify);
  }


// Replace m characters at pos by n characters
FXint FXText::replaceStyledText(FXint pos,FXint m,const FXchar *text,FXint n,FXint style,FXbool notify){
  FXTextChange textchange;
  if(n<0 || m<0 || pos<0 || length<pos+m){ fxerror("%s::replaceStyledText: bad argument.\n",getClassName()); }
  textchange.pos=pos;
  textchange.ndel=m;
  textchange.nins=n;
  textchange.ins=(FXchar*)text;
  allocElms(textchange.del,m);
  extractText(textchange.del,pos,m);
  replace(pos,m,text,n,style);
  if(notify && target){
    target->tryHandle(this,FXSEL(SEL_REPLACED,message),(void*)&textchange);
    target->tryHandle(this,FXSEL(SEL_CHANGED,message),(void*)(FXival)cursorpos);
    }
  freeElms(textchange.del);
  return n;
  }


// Replace m characters at pos by n characters
FXint FXText::replaceStyledText(FXint pos,FXint m,const FXString& text,FXint style,FXbool notify){
  return replaceStyledText(pos,m,text.text(),text.length(),style,notify);
  }


// Add text at the end
FXint FXText::appendText(const FXchar *text,FXint n,FXbool notify){
  return appendStyledText(text,n,0,notify);
  }


// Add text at the end
FXint FXText::appendText(const FXString& text,FXbool notify){
  return appendStyledText(text,0,notify);
  }


// Add text at the end
FXint FXText::appendStyledText(const FXchar *text,FXint n,FXint style,FXbool notify){
  FXTextChange textchange;
  if(n<0){ fxerror("%s::appendStyledText: bad argument.\n",getClassName()); }
  textchange.pos=length;
  textchange.ndel=0;
  textchange.nins=n;
  textchange.ins=(FXchar*)text;
  textchange.del=(FXchar*)"";
  replace(length,0,text,n,style);
  if(notify && target){
    target->tryHandle(this,FXSEL(SEL_INSERTED,message),(void*)&textchange);
    target->tryHandle(this,FXSEL(SEL_CHANGED,message),(void*)(FXival)cursorpos);
    }
  return n;
  }


// Add text at the end
FXint FXText::appendStyledText(const FXString& text,FXint style,FXbool notify){
  return appendStyledText(text.text(),text.length(),style,notify);
  }


// Insert some text at pos
FXint FXText::insertText(FXint pos,const FXchar *text,FXint n,FXbool notify){
  return insertStyledText(pos,text,n,0,notify);
  }


// Insert some text at pos
FXint FXText::insertText(FXint pos,const FXString& text,FXbool notify){
  return insertStyledText(pos,text.text(),text.length(),0,notify);
  }


// Insert some text at pos
FXint FXText::insertStyledText(FXint pos,const FXchar *text,FXint n,FXint style,FXbool notify){
  FXTextChange textchange;
  if(n<0 || pos<0 || length<pos){ fxerror("%s::insertStyledText: bad argument.\n",getClassName()); }
  textchange.pos=pos;
  textchange.ndel=0;
  textchange.nins=n;
  textchange.ins=(FXchar*)text;
  textchange.del=(FXchar*)"";
  replace(pos,0,text,n,style);
  if(notify && target){
    target->tryHandle(this,FXSEL(SEL_INSERTED,message),(void*)&textchange);
    target->tryHandle(this,FXSEL(SEL_CHANGED,message),(void*)(FXival)cursorpos);
    }
  return n;
  }


// Insert some text at pos
FXint FXText::insertStyledText(FXint pos,const FXString& text,FXint style,FXbool notify){
  return insertStyledText(pos,text.text(),text.length(),style,notify);
  }


// Remove some text at pos
FXint FXText::removeText(FXint pos,FXint n,FXbool notify){
  FXTextChange textchange;
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::removeText: bad argument.\n",getClassName()); }
  textchange.pos=pos;
  textchange.ndel=n;
  textchange.nins=0;
  textchange.ins=(FXchar*)"";
  allocElms(textchange.del,n);
  extractText(textchange.del,pos,n);
  replace(pos,n,NULL,0,0);
  if(notify && target){
    target->tryHandle(this,FXSEL(SEL_DELETED,message),(void*)&textchange);
    target->tryHandle(this,FXSEL(SEL_CHANGED,message),(void*)(FXival)cursorpos);
    }
  freeElms(textchange.del);
  return n;
  }


// Remove all text from the buffer
FXint FXText::clearText(FXbool notify){
  return removeText(0,length,notify);
  }


// Change style of text range
FXint FXText::changeStyle(FXint pos,FXint n,FXint style){
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::changeStyle: bad argument.\n",getClassName()); }
  if(sbuffer){
    if(pos+n<=gapstart){
      fillElms(sbuffer+pos,style,n);
      }
    else if(gapstart<=pos){
      fillElms(sbuffer+pos-gapstart+gapend,style,n);
      }
    else{
      fillElms(sbuffer+pos,style,gapstart-pos);
      fillElms(sbuffer+gapend,style,pos+n-gapstart);
      }
    updateRange(pos,pos+n);
    }
  return n;
  }


// Change style of text range from style-array
FXint FXText::changeStyle(FXint pos,const FXchar* style,FXint n){
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::changeStyle: bad argument.\n",getClassName()); }
  if(sbuffer && style){
    if(pos+n<=gapstart){
      copyElms(sbuffer+pos,style,n);
      }
    else if(gapstart<=pos){
      copyElms(sbuffer+gapend-gapstart+pos,style,n);
      }
    else{
      copyElms(sbuffer+pos,style,gapstart-pos);
      copyElms(sbuffer+gapend,style+gapstart-pos,pos+n-gapstart);
      }
    updateRange(pos,pos+n);
    }
  return n;
  }


// Change style of text range from style-array
FXint FXText::changeStyle(FXint pos,const FXString& style){
  return changeStyle(pos,style.text(),style.length());
  }


// Grab range of text
void FXText::extractText(FXchar *text,FXint pos,FXint n) const {
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::extractText: bad argument.\n",getClassName()); }
  if(pos+n<=gapstart){
    copyElms(text,buffer+pos,n);
    }
  else if(gapstart<=pos){
    copyElms(text,buffer+gapend-gapstart+pos,n);
    }
  else{
    copyElms(text,buffer+pos,gapstart-pos);
    copyElms(text+gapstart-pos,buffer+gapend,pos+n-gapstart);
    }
  }


// Return n bytes of contents of text buffer from position pos
FXString FXText::extractText(FXint pos,FXint n) const {
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::extractText: bad argument.\n",getClassName()); }
  FXString result;
  result.length(n);
  if(pos+n<=gapstart){
    result.replace(0,n,buffer+pos,n);
    }
  else if(gapstart<=pos){
    result.replace(0,n,buffer+gapend-gapstart+pos,n);
    }
  else{
    result.replace(0,gapstart-pos,buffer+pos,gapstart-pos);
    result.replace(gapstart-pos,pos+n-gapstart,buffer+gapend,pos+n-gapstart);
    }
  return result;
  }


// Grab range of style
void FXText::extractText(FXString& text,FXint pos,FXint n) const {
  text.length(n);
  extractText(text.text(),pos,n);
  }


// Grab range of style
void FXText::extractStyle(FXchar *style,FXint pos,FXint n) const {
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::extractStyle: bad argument.\n",getClassName()); }
  if(sbuffer){
    if(pos+n<=gapstart){
      copyElms(style,sbuffer+pos,n);
      }
    else if(gapstart<=pos){
      copyElms(style,sbuffer+gapend-gapstart+pos,n);
      }
    else{
      copyElms(style,sbuffer+pos,gapstart-pos);
      copyElms(style+gapstart-pos,sbuffer+gapend,pos+n-gapstart);
      }
    }
  }


// Return n bytes of style info from buffer from position pos
FXString FXText::extractStyle(FXint pos,FXint n) const {
  if(n<0 || pos<0 || length<pos+n){ fxerror("%s::extractStyle: bad argument.\n",getClassName()); }
  FXString result;
  if(sbuffer){
    result.length(n);
    if(pos+n<=gapstart){
      result.replace(0,n,sbuffer+pos,n);
      }
    else if(gapstart<=pos){
      result.replace(0,n,sbuffer+gapend-gapstart+pos,n);
      }
    else{
      result.replace(0,gapstart-pos,sbuffer+pos,gapstart-pos);
      result.replace(gapstart-pos,pos+n-gapstart,sbuffer+gapend,pos+n-gapstart);
      }
    }
  return result;
  }


// Grab range of style
void FXText::extractStyle(FXString& style,FXint pos,FXint n) const {
  style.length(n);
  extractStyle(style.text(),pos,n);
  }


// Retrieve text into buffer
void FXText::getText(FXchar* text,FXint n) const {
  extractText(text,0,n);
  }


// Retrieve text into buffer
void FXText::getText(FXString& text) const {
  extractText(text,0,getLength());
  }


// We return a constant copy of the buffer
FXString FXText::getText() const {
  return extractText(0,getLength());
  }


// Get selected text
FXString FXText::getSelectedText() const {
  FXString result;
  if(select.startpos<=select.endpos){
    if(select.startcol<=select.endcol){
      result=extractTextBlock(select.startpos,select.endpos,select.startcol,select.endcol);
      }
    else{
      result=extractText(select.startpos,select.endpos-select.startpos);
      }
    }
  return result;
  }


/*******************************************************************************/

// Select all text
FXbool FXText::selectAll(FXbool notify){
  return setSelection(0,length,notify);
  }


// Set selection
FXbool FXText::setSelection(FXint pos,FXint len,FXbool notify){
  FXDragType types[4]={stringType,textType,utf8Type,utf16Type};
  FXint spos=validPos(pos);
  FXint epos=validPos(pos+len);
  if(select.startpos!=spos || select.endpos!=epos){
    FXint what[4];

    // Update affected areas
    if((epos<=select.startpos) || (select.endpos<=spos)){
      updateRange(select.startpos,select.endpos);
      updateRange(spos,epos);
      }
    else{
      updateRange(select.startpos,spos);
      updateRange(select.endpos,epos);
      }

    // Release selection
    if(spos>=epos){
      if(hasSelection()) releaseSelection();
      if(notify && target){
        what[0]=select.startpos;
        what[1]=select.endpos-select.startpos;
        what[2]=select.startcol;
        what[3]=select.endcol-select.startcol;
        target->tryHandle(this,FXSEL(SEL_DESELECTED,message),(void*)what);
        }
      select.startpos=0;
      select.endpos=-1;
      select.startcol=0;
      select.endcol=-1;
      }

    // Acquire selection
    else{
      if(!hasSelection()) acquireSelection(types,4);
      if(notify && target){
        what[0]=select.startpos;
        what[1]=select.endpos-select.startpos;
        what[2]=select.startcol;
        what[3]=select.endcol-select.startcol;
        target->tryHandle(this,FXSEL(SEL_SELECTED,message),(void*)what);
        }
      select.startpos=spos;
      select.endpos=epos;
      select.startcol=0;
      select.endcol=-1;
      }
    return true;
    }
  return false;
  }


// Extend selection
FXbool FXText::extendSelection(FXint pos,FXuint sel,FXbool notify){
  register FXint p=validPos(pos),ss=0,se=0;
  switch(sel){
    case SelectChars:                   // Selecting characters
      if(p<=anchorpos){
        ss=p;
        se=anchorpos;
        }
      else{
        ss=anchorpos;
        se=p;
        }
      break;
    case SelectWords:                   // Selecting words
      if(p<=anchorpos){
        ss=wordStart(p);
        se=wordEnd(anchorpos);
        }
      else{
        ss=wordStart(anchorpos);
        se=wordEnd(p);
        }
      break;
    case SelectRows:                    // Selecting rows
      if(p<=anchorpos){
        ss=rowStart(p);
        se=nextRow(anchorpos);
        }
      else{
        ss=rowStart(anchorpos);
        se=nextRow(p);
        }
      break;
    case SelectLines:                   // Selecting lines
      if(p<=anchorpos){
        ss=lineStart(p);
        se=nextLine(anchorpos);
        }
      else{
        ss=lineStart(anchorpos);
        se=nextLine(p);
        }
      break;
    }
  return setSelection(ss,se-ss,notify);
  }


// Select block of characters within given box
FXbool FXText::setBlockSelection(FXint trow,FXint lcol,FXint brow,FXint rcol,FXbool notify){
  FXDragType types[4]={stringType,textType,utf8Type,utf16Type};
  FXint spos=lineStart(posFromRow(trow));
  FXint epos=lineEnd(posFromRow(brow));
  if(select.startpos!=spos || select.endpos!=epos || select.startcol!=lcol || select.endcol!=rcol){
    FXint what[4];

    // Update affected areas
    updateLines(select.startpos,select.endpos);
    updateLines(spos,epos);

    // Release selection
    if(spos>epos || lcol>rcol){
      if(hasSelection()) releaseSelection();
      if(notify && target){
        what[0]=select.startpos;
        what[1]=select.endpos-select.startpos;
        what[2]=select.startcol;
        what[3]=select.endcol-select.startcol;
        target->tryHandle(this,FXSEL(SEL_DESELECTED,message),(void*)what);
        }
      select.startpos=0;
      select.endpos=-1;
      select.startcol=0;
      select.endcol=-1;
      }

    // Acquire selection
    else{
      if(!hasSelection()) acquireSelection(types,4);
      if(notify && target){
        what[0]=select.startpos;
        what[1]=select.endpos-select.startpos;
        what[2]=select.startcol;
        what[3]=select.endcol-select.startcol;
        target->tryHandle(this,FXSEL(SEL_SELECTED,message),(void*)what);
        }
      select.startpos=spos;
      select.endpos=epos;
      select.startcol=lcol;
      select.endcol=rcol;
      }
    FXTRACE((140,"select: startpos=%d endpos=%d startcol=%d endcol=%d\n",select.startpos,select.endpos,select.startcol,select.endcol));
    return true;
    }
  return false;
  }


// Extend primary selection from anchor to given row, column
FXbool FXText::extendBlockSelection(FXint row,FXint col,FXbool notify){
  FXint trow,brow,lcol,rcol;
  FXMINMAX(trow,brow,anchorrow,row);
  FXMINMAX(lcol,rcol,anchorvcol,col);
  return setBlockSelection(trow,lcol,brow,rcol,notify);
  }


// Kill the selection
FXbool FXText::killSelection(FXbool notify){
  if(select.startpos<=select.endpos){
    FXint what[4];
    if(hasSelection()) releaseSelection();
    if(notify && target){
      what[0]=select.startpos;
      what[1]=select.endpos-select.startpos;
      what[2]=select.startcol;
      what[3]=select.endcol-select.startcol;
      target->tryHandle(this,FXSEL(SEL_DESELECTED,message),(void*)what);
      }
    updateRange(select.startpos,select.endpos);
    select.startpos=0;
    select.endpos=-1;
    select.startcol=0;
    select.endcol=-1;
    return true;
    }
  return false;
  }


// Copy selection to clipboard
FXbool FXText::copySelection(){
  FXDragType types[4]={stringType,textType,utf8Type,utf16Type};
  if(select.startpos<=select.endpos){
    if(acquireClipboard(types,ARRAYNUMBER(types))){
      clipped=getSelectedText();
      return true;
      }
    }
  return false;
  }


// Copy selection to clipboard and delete it
FXbool FXText::cutSelection(FXbool notify){
  if(copySelection()){
    return deleteSelection(notify);
    }
  return false;
  }


// Delete selection
FXbool FXText::deleteSelection(FXbool notify){
  if(select.startpos<=select.endpos){
    if(select.startcol<=select.endcol){
      FXint pos,m;
      m=removeTextBlock(select.startpos,select.endpos,select.startcol,select.endcol,notify);
      pos=posFromColumn(lineStart(select.startpos+m),select.startcol);
      moveCursor(pos,notify);   // FIXME keep cursor on same line, but at end of column
      }
    else{
      removeText(select.startpos,select.endpos-select.startpos,notify);
      moveCursor(cursorpos,notify);
      }
    return true;
    }
  return false;
  }


// Delete pending selection
FXbool FXText::deletePendingSelection(FXbool notify){
  return isPosSelected(cursorpos,cursorvcol) && deleteSelection(notify);
  }


// Paste primary ("middle-mouse") selection
FXbool FXText::pasteSelection(FXbool notify){

  // Don't paste inside selection
  if((select.startpos>select.endpos) || (cursorpos<=select.startpos) || (select.endpos<=cursorpos)){
    FXString string;

    // Try UTF-8, then UTF-16, then 8859-1
    if(getDNDData(FROM_SELECTION,utf8Type,string) || getDNDData(FROM_SELECTION,utf16Type,string) || getDNDData(FROM_SELECTION,stringType,string)){
      FXint start=cursorpos;
      FXint end=cursorpos;

      // Overstrike mode, extent
      if(isOverstrike()){
        end=overstruck(start,end,string.text(),string.length());
        }

      // Replace text and move cursor
      replaceText(start,end-start,string,notify);
      makePositionVisible(cursorpos);
      setCursorPos(cursorpos,notify);
      setAnchorPos(cursorpos);
      flashMatching();
      return true;
      }
    }
  return false;
  }


// Paste clipboard
FXbool FXText::pasteClipboard(FXbool notify){
  FXString string;

  // Try UTF-8, then UTF-16, then 8859-1
  if(getDNDData(FROM_CLIPBOARD,utf8Type,string) || getDNDData(FROM_CLIPBOARD,utf16Type,string) || getDNDData(FROM_CLIPBOARD,stringType,string)){
    FXint pos=cursorpos,cols,m;

    // Convert newlines
#ifdef WIN32
    dosToUnix(string);
#endif

    // Replace selected characters
    if(isPosSelected(cursorpos,cursorvcol)){
      if(select.startcol<=select.endcol){
        cols=maxcolumns(string.text(),string.text()+string.length(),tabcolumns);
        m=replaceTextBlock(select.startpos,select.endpos,select.startcol,select.endcol,string,notify);
        pos=posFromColumn(lineStart(select.startpos+m),select.startcol+cols);
        }
      else{
        m=replaceText(select.startpos,select.endpos-select.startpos,string,notify);
        pos=select.startpos+m;
        }
      }

    // Insert or overstrike
    else{

      // Overstrike
      if(isOverstrike()){
        m=overstruck(pos,pos,string.text(),string.length());
        }
      else{
        m=pos;
        }
      m=replaceText(pos,m-pos,string,notify);
      pos=pos+m;
      }

    // Move cursor to pos
    moveCursor(pos,notify);
    return true;
    }
  return false;
  }


// Replace selection by other text
FXbool FXText::replaceSelection(const FXString& text,FXbool notify){
  if(select.startpos<=select.endpos){
    FXint pos=select.startpos,m,cols;
    if(select.startcol<=select.endcol){
      cols=maxcolumns(text.text(),text.text()+text.length(),tabcolumns);
      m=replaceTextBlock(select.startpos,select.endpos,select.startcol,select.endcol,text,notify);
      pos=posFromColumn(lineStart(pos+m),select.startcol+cols);
      }
    else{
      m=replaceText(select.startpos,select.endpos-select.startpos,text,notify);
      pos=pos+m;
      }
    moveCursor(pos,notify);
    return true;
    }
  return false;
  }


// Set highlight
FXbool FXText::setHighlight(FXint pos,FXint len){
  register FXint spos=validPos(pos);
  register FXint epos=validPos(pos+len);
  if(spos!=hilite.startpos || epos!=hilite.endpos){
    if(epos<=hilite.startpos || hilite.endpos<=spos){
      updateRange(hilite.startpos,hilite.endpos);
      updateRange(spos,epos);
      }
    else{
      updateRange(hilite.startpos,spos);
      updateRange(hilite.endpos,epos);
      }
    hilite.startpos=spos;
    hilite.endpos=epos;
    hilite.startcol=0;
    hilite.endcol=-1;
    return true;
    }
  return false;
  }


// Unhighlight the text
FXbool FXText::killHighlight(){
  if(hilite.startpos<=hilite.endpos){
    updateRange(hilite.startpos,hilite.endpos);
    hilite.startpos=0;
    hilite.endpos=-1;
    hilite.startcol=0;
    hilite.endcol=-1;
    return true;
    }
  return false;
  }

/*******************************************************************************/

// Draw the cursor
void FXText::drawCursor(FXuint state){
  if((state^flags)&FLAG_CARET){
    if(xid){
      FXDCWindow dc(this);
      if(state&FLAG_CARET)
        paintCursor(dc);
      else
        eraseCursor(dc);
      }
    flags^=FLAG_CARET;
    }
  }


// Paint cursor glyph
void FXText::paintCursor(FXDCWindow& dc) const {
  FXint th,tw,cursorx,cursory;
  FXwchar c;
  th=font->getFontHeight();
  cursory=getVisibleY()+margintop+pos_y+cursorrow*th;
  if(getVisibleY()+margintop<cursory+th && cursory<=getVisibleY()+getVisibleHeight()-marginbottom){
    FXASSERT(toprow<=cursorrow && cursorrow<toprow+nvisrows);
    tw=font->getCharWidth((cursorpos<length) && ((c=getChar(cursorpos))>=' ')?c:' ');
    cursorx=getVisibleX()+marginleft+pos_x+xoffset(visrows[cursorrow-toprow],cursorpos)-1;
    if(getVisibleX()<=cursorx+tw+2 && cursorx-2<=getVisibleX()+getVisibleWidth()){
      dc.setClipRectangle(getVisibleX(),getVisibleY(),getVisibleWidth(),getVisibleHeight());
      if(0<dc.getClipWidth() && 0<dc.getClipHeight()){
        dc.setForeground(cursorColor);
        if(options&TEXT_OVERSTRIKE){
          dc.drawRectangle(cursorx,cursory,tw,th-1);
          }
        else{
          dc.fillRectangle(cursorx,cursory,2,th);
          dc.fillRectangle(cursorx-2,cursory,6,1);
          dc.fillRectangle(cursorx-2,cursory+th-1,6,1);
          }
        }
      }
    }
  }


// Erase cursor glyph
void FXText::eraseCursor(FXDCWindow& dc) const {
  FXint th,tw,cursorx,cursory,cx,cy,ch,cw;
  FXwchar c;
  th=font->getFontHeight();
  cursory=getVisibleY()+margintop+pos_y+cursorrow*th;
  if(getVisibleY()+margintop<cursory+th && cursory<=getVisibleY()+getVisibleHeight()-marginbottom){
    FXASSERT(0<=cursorrow-toprow && cursorrow-toprow<nvisrows);
    tw=font->getCharWidth((cursorpos<length) && ((c=getChar(cursorpos))>=' ')?c:' ');
    cursorx=getVisibleX()+marginleft+pos_x+xoffset(visrows[cursorrow-toprow],cursorpos)-1;
    if(getVisibleX()<=cursorx+tw+2 && cursorx-2<=getVisibleX()+getVisibleWidth()){
      dc.setClipRectangle(getVisibleX(),getVisibleY(),getVisibleWidth(),getVisibleHeight());
      if(0<dc.getClipWidth() && 0<dc.getClipHeight()){
        dc.setFont(font);
        dc.setForeground(backColor);
        dc.fillRectangle(cursorx-2,cursory,tw+4,th);
        cx=FXMAX(cursorx-2,getVisibleX()+marginleft);
        cy=getVisibleY()+margintop;
        cw=FXMIN(cursorx+tw+2,getVisibleX()+getVisibleWidth()-marginright)-cx;
        ch=getVisibleHeight()-margintop-marginbottom;
        dc.setClipRectangle(cx,cy,cw,ch);
        FXASSERT(toprow<=cursorrow && cursorrow<toprow+nvisrows);
        drawTextRow(dc,cursorrow);
        }
      }
    }
  }


// Erase cursor overhang outside of margins
void FXText::eraseCursorOverhang(){
  FXint th,tw,cursorx,cursory;
  FXwchar c;
  th=font->getFontHeight();
  cursory=getVisibleY()+margintop+pos_y+cursorrow*th;
  if(getVisibleY()+margintop<cursory+th && cursory<=getVisibleY()+getVisibleHeight()-marginbottom){
    FXASSERT(0<=cursorrow-toprow && cursorrow-toprow<nvisrows);
    tw=font->getCharWidth((cursorpos<length) && ((c=getChar(cursorpos))>=' ')?c:' ');
    cursorx=getVisibleX()+marginleft+pos_x+xoffset(visrows[cursorrow-toprow],cursorpos)-1;
    if(getVisibleX()<=cursorx+tw+2 && cursorx-2<=getVisibleX()+getVisibleWidth()){
      FXDCWindow dc(this);
      if(cursorx-2<=getVisibleX()+marginleft && getVisibleX()<=cursorx+tw+2){
        dc.setForeground(backColor);
        dc.fillRectangle(getVisibleX(),cursory,marginleft,th);
        }
      if(getVisibleX()+getVisibleWidth()-marginright<=cursorx+tw+2 && cursorx-2<=getVisibleX()+getVisibleWidth()){
        dc.setForeground(backColor);
        dc.fillRectangle(getVisibleX()+getVisibleWidth()-marginright,cursory,marginright,th);
        }
      if(cursory<=getVisibleY()+margintop && getVisibleY()<=cursory+th){
        dc.setForeground(backColor);
        dc.fillRectangle(cursorx-2,getVisibleY(),tw+4,margintop);
        }
      if(getVisibleY()+getVisibleHeight()-marginbottom<=cursory+th && cursory<getVisibleY()+getVisibleHeight()){
        dc.setForeground(backColor);
        dc.fillRectangle(cursorx-2,getVisibleY()+getVisibleHeight()-marginbottom,tw+4,marginbottom);
        }
      }
    }
  }

/*******************************************************************************/

// Draw fragment of text in given style
void FXText::drawBufferText(FXDCWindow& dc,FXint x,FXint y,FXint,FXint,FXint pos,FXint n,FXuint style) const {
  register FXuint index=(style&STYLE_MASK);
  register FXuint usedstyle=style;                                              // Style flags from style buffer
  register FXColor color;
  FXchar str[2];
  color=0;
  if(hilitestyles && index){                                                    // Get colors from style table
    usedstyle=hilitestyles[index-1].style;                                      // Style flags now from style table
    if(style&STYLE_SELECTED) color=hilitestyles[index-1].selectForeColor;
    else if(style&STYLE_HILITE) color=hilitestyles[index-1].hiliteForeColor;
    if(color==0) color=hilitestyles[index-1].normalForeColor;                   // Fall back on normal foreground color
    }
  if(color==0){                                                                 // Fall back to default style
    if(style&STYLE_SELECTED) color=seltextColor;
    else if(style&STYLE_HILITE) color=hilitetextColor;
    if(color==0) color=textColor;                                               // Fall back to normal text color
    }
  dc.setForeground(color);
  if(style&STYLE_CONTROL){
    y+=font->getFontAscent();
    str[0]='^';
    while(pos<gapstart && 0<n){
      str[1]=buffer[pos]|0x40;
      dc.drawText(x,y,str,2);
      if(usedstyle&STYLE_BOLD) dc.drawText(x+1,y,str,2);
      x+=font->getTextWidth(str,2);
      pos++;
      n--;
      }
    while(0<n){
      str[1]=buffer[pos-gapstart+gapend]|0x40;
      dc.drawText(x,y,str,2);
      if(usedstyle&STYLE_BOLD) dc.drawText(x+1,y,str,2);
      x+=font->getTextWidth(str,2);
      pos++;
      n--;
      }
    }
  else{
    y+=font->getFontAscent();
    if(pos+n<=gapstart){
      dc.drawText(x,y,&buffer[pos],n);
      if(usedstyle&STYLE_BOLD) dc.drawText(x+1,y,&buffer[pos],n);
      }
    else if(pos>=gapstart){
      dc.drawText(x,y,&buffer[pos-gapstart+gapend],n);
      if(usedstyle&STYLE_BOLD) dc.drawText(x+1,y,&buffer[pos-gapstart+gapend],n);
      }
    else{
      dc.drawText(x,y,&buffer[pos],gapstart-pos);
      if(usedstyle&STYLE_BOLD) dc.drawText(x+1,y,&buffer[pos],gapstart-pos);
      x+=font->getTextWidth(&buffer[pos],gapstart-pos);
      dc.drawText(x,y,&buffer[gapend],pos+n-gapstart);
      if(usedstyle&STYLE_BOLD) dc.drawText(x+1,y,&buffer[gapend],pos+n-gapstart);
      }
    }
  }


// Fill fragment of background in given style
void FXText::fillBufferRect(FXDCWindow& dc,FXint x,FXint y,FXint w,FXint h,FXuint style) const {
  register FXuint index=(style&STYLE_MASK);
  register FXuint usedstyle=style;                              // Style flags from style buffer
  register FXColor bgcolor,fgcolor;
  bgcolor=fgcolor=0;
  if(hilitestyles && index){                                    // Get colors from style table
    usedstyle=hilitestyles[index-1].style;                      // Style flags now from style table
    if(style&STYLE_SELECTED){
      bgcolor=hilitestyles[index-1].selectBackColor;
      fgcolor=hilitestyles[index-1].selectForeColor;
      }
    else if(style&STYLE_HILITE){
      bgcolor=hilitestyles[index-1].hiliteBackColor;
      fgcolor=hilitestyles[index-1].hiliteForeColor;
      }
    else if(style&STYLE_ACTIVE){
      bgcolor=hilitestyles[index-1].activeBackColor;
      }
    else{
      bgcolor=hilitestyles[index-1].normalBackColor;
      }
    if(fgcolor==0){                                             // Fall back to normal foreground color
      fgcolor=hilitestyles[index-1].normalForeColor;
      }
    }
  if(bgcolor==0){                                               // Fall back to default background colors
    if(style&STYLE_SELECTED) bgcolor=selbackColor;
    else if(style&STYLE_HILITE) bgcolor=hilitebackColor;
    else if(style&STYLE_ACTIVE) bgcolor=activebackColor;
    else bgcolor=backColor;
    }
  if(fgcolor==0){                                               // Fall back to default foreground colors
    if(style&STYLE_SELECTED) fgcolor=seltextColor;
    else if(style&STYLE_HILITE) fgcolor=hilitetextColor;
    if(fgcolor==0) fgcolor=textColor;                           // Fall back to text color
    }
  dc.setForeground(bgcolor);
  dc.fillRectangle(x,y,w,h);
  if(style&STYLE_INSERT){                                       // Vertical insertion point
    dc.setForeground(cursorColor);                              // Use cursor color for now
    dc.fillRectangle(x,y,1,h);
    }
  if(usedstyle&STYLE_UNDERLINE){
    dc.setForeground(fgcolor);
    dc.fillRectangle(x,y+font->getFontAscent()+1,w,1);
    }
  if(usedstyle&STYLE_STRIKEOUT){
    dc.setForeground(fgcolor);
    dc.fillRectangle(x,y+font->getFontAscent()/2,w,1);
    }
  }


// Obtain text style given line range, row, column, and position
// Note that for block selections, the column may be outside the text
FXuint FXText::styleOf(FXint beg,FXint end,FXint row,FXint col,FXint pos) const {
  FXuint style=0;
  if(beg<end){

    // Selected range or block
    if(select.startcol>select.endcol){
      if(select.startpos<=pos && pos<select.endpos) style|=STYLE_SELECTED;
      }
    else if(select.startpos<=pos && pos<=select.endpos){
      if(select.startcol<=col && col<select.endcol) style|=STYLE_SELECTED;
      if(select.startcol==col && select.endcol==col) style|=STYLE_INSERT;
      }

    // Highlighted range or block
    if(hilite.startcol>hilite.endcol){
      if(hilite.startpos<=pos && pos<hilite.endpos) style|=STYLE_HILITE;
      }
    else if(hilite.startpos<=pos && pos<=hilite.endpos){
      if(hilite.startcol<=col && col<hilite.endcol) style|=STYLE_HILITE;
      }

    // Current active line
    if((row==cursorrow) && (options&TEXT_SHOWACTIVE)) style|=STYLE_ACTIVE;

    // Inside text
    if(pos<end){

      // Get character
      FXuchar c=getByte(pos);

      // Get value from style buffer
      if(sbuffer) style|=getStyle(pos);

      // Tab or whitespace
      if(c=='\t') return style;
      if(c==' ') return style;

      // Control codes
      if(c<' ') style|=STYLE_CONTROL;

      // Normal character
      style|=STYLE_TEXT;
      }
    }
  return style;
  }


// Draw line of text from the buffer, skipping over the parts outside
// of the current clip rectangle.
void FXText::drawTextRow(FXDCWindow& dc,FXint row) const {
  FXint spacew=font->getCharWidth(' ');
  FXint caretw=font->getCharWidth('^');
  FXint th=font->getFontHeight();
  FXint tx=getVisibleX()+marginleft+pos_x;
  FXint ty=getVisibleY()+margintop+pos_y+row*th;
  FXint leftclip=dc.getClipX();
  FXint riteclip=dc.getClipX()+dc.getClipWidth();
  FXint linebeg=visrows[row-toprow];
  FXint lineend=visrows[row-toprow+1];
  FXint linebreak=lineend;
  FXint tcol=0,twid=0,tadj=0;
  FXint cw,cc,pc,cx,px,cp,pp;
  FXuint curstyle,newstyle;
  FXwchar c;

  FXASSERT(toprow<=row && row<toprow+nvisrows);
  FXASSERT(0<=linebeg && lineend<=length);

  // Back off past break-character, i.e. space or newline (if any)
  if(linebeg<lineend){
    pp=dec(lineend);
    c=getChar(pp);
    if(Unicode::isSpace(c)) linebreak=pp;
    }

  // Reset running variables
  cc=0;
  cx=tx;
  cp=linebeg;

  // Scan forward to get past left edge
  do{
    px=cx;
    pc=cc;
    pp=cp;
    if(cp>=linebreak){                          // Character past end of line
      cx+=spacew;
      cc+=1;
      continue;
      }
    c=getChar(cp);
    if(' '<=c){                                 // Normal character
      cx+=font->getCharWidth(c);
      cc+=1;
      cp+=getCharLen(cp);
      continue;
      }
    if(c=='\t'){                                // Tab character
      cx+=tabwidth-(cx-tx)%tabwidth;
      cc+=tabcolumns-cc%tabcolumns;
      cp+=1;
      continue;
      }
    cx+=caretw+font->getCharWidth(c|0x40);      // Control character
    cc+=1;
    cp+=1;
    }
  while(cx<leftclip);

  // Roll back to just before edge
  cx=px;
  cc=pc;
  cp=pp;

  // First style to display
  curstyle=styleOf(linebeg,lineend,row,cc,cp);

  // Draw segments of uniformly styled text
  do{
    newstyle=styleOf(linebeg,lineend,row,cc,cp);
    if(newstyle!=curstyle){                     // Found a style change!
      fillBufferRect(dc,px,ty,cx-px,th,curstyle);
      if(curstyle&STYLE_TEXT) drawBufferText(dc,px,ty,cx-px,th,pp,cp-pp,curstyle);
      curstyle=newstyle;
      pp=cp;
      pc=cc;
      px=cx;
      }
    if(cp>=linebreak){                          // Character past end of line
      cx+=spacew;
      cc+=1;
      continue;
      }
    c=getChar(cp);
    if(' '<=c){                                 // Normal character
      cx+=font->getCharWidth(c);
      cc+=1;
      cp+=getCharLen(cp);
      continue;
      }
    if(c=='\t'){                                // Tab character
      if(tcol==0){
        cw=tabwidth-(cx-tx)%tabwidth;
        tcol=tabcolumns-cc%tabcolumns;
        twid=cw/tcol;
        tadj=cw-twid*tcol;
        }
      cx+=twid+(tadj>0);                        // Mete out columns comprising the tab character
      tcol-=1;
      tadj-=1;
      cc+=1;
      cp+=(tcol==0);
      continue;
      }
    cx+=caretw+font->getCharWidth(c|0x40);      // Control character
    cc+=1;
    cp+=1;
    }
  while(cx<riteclip);

  // Draw unfinished fragment
  fillBufferRect(dc,px,ty,cx-px,th,curstyle);
  if(curstyle&STYLE_TEXT) drawBufferText(dc,px,ty,cx-px,th,pp,cp-pp,curstyle);
  }


// Repaint lines of text
// Erase margins, then draw text one line at a time to reduce flicker.
// Only draw if intersection of bar area and dirty rectangle is non-empty
void FXText::drawContents(FXDCWindow& dc) const {
  register FXint vx=getVisibleX();
  register FXint vy=getVisibleY();
  register FXint vw=getVisibleWidth();
  register FXint vh=getVisibleHeight();
  dc.setClipRectangle(vx,vy,vw,vh);
  if(0<dc.getClipWidth() && 0<dc.getClipHeight()){
    register FXint th,row,trow,brow;
    dc.setForeground(backColor);
    if(dc.getClipY()<=vy+margintop){
      dc.fillRectangle(vx,vy,vw,margintop);
      }
    if(dc.getClipY()+dc.getClipHeight()>=vy+vh-marginbottom){
      dc.fillRectangle(vx,vy+vh-marginbottom,vw,marginbottom);
      }
    if(dc.getClipX()<vx+marginleft){
      dc.fillRectangle(vx,vy+margintop,marginleft,vh-margintop-marginbottom);
      }
    if(dc.getClipX()+dc.getClipWidth()>=vx+vw-marginright){
      dc.fillRectangle(vx+vw-marginright,vy+margintop,marginright,vh-margintop-marginbottom);
      }
    th=font->getFontHeight();
    trow=(dc.getClipY()-pos_y-vy-margintop)/th;
    brow=(dc.getClipY()+dc.getClipHeight()-pos_y-vy-margintop)/th;
    if(trow<=toprow) trow=toprow;
    if(brow>=toprow+nvisrows) brow=toprow+nvisrows-1;
    dc.setClipRectangle(vx+marginleft,vy+margintop,vw-marginright-marginleft,vh-margintop-marginbottom);
    for(row=trow; row<=brow; row++){
      drawTextRow(dc,row);
      }
    }
  }


// Repaint line numbers
// Erase and redraw number one at a time, instead of erasing all background
// and then drawing numbers on top; this leads to less flicker.
// Only draw if intersection of bar area and dirty rectangle is non-empty
void FXText::drawNumbers(FXDCWindow& dc) const {
  register FXint vx=getVisibleX();
  register FXint vy=getVisibleY();
  register FXint vh=getVisibleHeight();
  dc.setClipRectangle(0,vy,vx,vh);
  if(0<dc.getClipWidth() && 0<dc.getClipHeight()){
    register FXint tw,th,trow,brow,row,n;
    FXchar number[20];
    dc.setForeground(barColor);
    if(dc.getClipY()<=vy+margintop){
      dc.fillRectangle(0,vy,vx,margintop);
      }
    if(dc.getClipY()+dc.getClipHeight()>=vy+vh-marginbottom){
      dc.fillRectangle(0,vy+vh-marginbottom,vx,marginbottom);
      }
    th=font->getFontHeight();
    trow=(dc.getClipY()-pos_y-vy-margintop)/th;
    brow=(dc.getClipY()+dc.getClipHeight()-pos_y-vy-margintop)/th;
    if(trow<=toprow) trow=toprow;
    if(brow>=toprow+nvisrows) brow=toprow+nvisrows;
    dc.setClipRectangle(0,vy+margintop,vx,vh-margintop-marginbottom);
    for(row=trow; row<=brow; row++){
      n=__snprintf(number,sizeof(number),"%d",row+1);
      tw=font->getTextWidth(number,n);
      dc.setForeground(barColor);
      dc.fillRectangle(0,pos_y+vy+margintop+row*th,vx,th);
      dc.setForeground(numberColor);
      dc.drawText(vx-tw,pos_y+vy+margintop+row*th+font->getFontAscent(),number,n);
      }
    }
  }


// Repaint the row
void FXText::updateRow(FXint row) const {
  if(toprow<=row && row<=toprow+nvisrows){
    update(getVisibleX(),getVisibleY()+margintop+pos_y+row*font->getFontHeight(),getVisibleWidth(),font->getFontHeight());
    }
  }


// Update whole lines
void FXText::updateLines(FXint startpos,FXint endpos) const {
  FXint b,e,tr,br,ty,by;
  FXMINMAX(b,e,startpos,endpos);
  if(b<=visrows[nvisrows] && visrows[0]<e){
    if(b<visrows[0]) b=visrows[0];
    if(e>visrows[nvisrows-1]) e=visrows[nvisrows-1];
    tr=rowFromPos(b);
    br=rowFromPos(e);
    ty=getVisibleY()+margintop+pos_y+tr*font->getFontHeight();
    by=getVisibleY()+margintop+pos_y+br*font->getFontHeight()+font->getFontHeight();
    update(getVisibleX(),ty,getVisibleWidth(),by-ty);
    }
  }


// Repaint text range
void FXText::updateRange(FXint startpos,FXint endpos) const {
  FXint vx,vy,vw,b,e,tr,br,lx,rx,ty,by;
  FXMINMAX(b,e,startpos,endpos);
  if(b<=visrows[nvisrows] && visrows[0]<e){
    if(b<visrows[0]) b=visrows[0];
    if(e>visrows[nvisrows-1]) e=visrows[nvisrows-1];
    vx=getVisibleX();
    vy=getVisibleY();
    vw=getVisibleWidth();
    tr=rowFromPos(b);
    br=rowFromPos(e);
    if(tr==br){
      ty=pos_y+vy+margintop+tr*font->getFontHeight();
      by=ty+font->getFontHeight();
      lx=vx+pos_x+marginleft+xoffset(visrows[tr-toprow],b);
      if(e<=(visrows[tr-toprow+1]-1))
        rx=vx+pos_x+marginleft+xoffset(visrows[tr-toprow],e);
      else
        rx=vx+vw;
      }
    else{
      ty=vy+pos_y+margintop+tr*font->getFontHeight();
      by=vy+pos_y+margintop+br*font->getFontHeight()+font->getFontHeight();
      lx=vx;
      rx=lx+vw;
      }
    update(lx,ty,rx-lx,by-ty);
    }
  }


// Draw the text
long FXText::onPaint(FXObject*,FXSelector,void* ptr){
  FXDCWindow dc(this,(FXEvent*)ptr);

  // Set font
  dc.setFont(font);

//dc.setForeground(FXRGB(255,0,0));
//dc.fillRectangle(0,0,width,height);

  // Paint text
  drawContents(dc);

  // Paint line numbers if turned on
  if(barwidth){
    drawNumbers(dc);
    }

  // Paint cursor
  if(flags&FLAG_CARET){
    paintCursor(dc);
    }
  return 1;
  }

/*******************************************************************************/

// Blink the cursor
long FXText::onBlink(FXObject*,FXSelector,void*){
  drawCursor(blink);
  blink^=FLAG_CARET;
  getApp()->addTimeout(this,ID_BLINK,getApp()->getBlinkSpeed());
  return 0;
  }


// Flash matching brace
long FXText::onFlash(FXObject*,FXSelector,void*){
  killHighlight();
  return 0;
  }


// Start motion timer while in this window
long FXText::onEnter(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onEnter(sender,sel,ptr);
  getApp()->addTimeout(this,ID_TIPTIMER,getApp()->getMenuPause());
  return 1;
  }


// Stop motion timer when leaving window
long FXText::onLeave(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onLeave(sender,sel,ptr);
  getApp()->removeTimeout(this,ID_TIPTIMER);
  return 1;
  }


// Gained focus
long FXText::onFocusIn(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onFocusIn(sender,sel,ptr);
  if(isEditable()){
    getApp()->addTimeout(this,ID_BLINK,getApp()->getBlinkSpeed());
    drawCursor(FLAG_CARET);
    }
  return 1;
  }


// Lost focus
long FXText::onFocusOut(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onFocusOut(sender,sel,ptr);
  if(isEditable()){
    getApp()->removeTimeout(this,ID_BLINK);
    drawCursor(0);
    }
  flags|=FLAG_UPDATE;
  return 1;
  }

/*******************************************************************************/

// Update value from a message
long FXText::onCmdSetStringValue(FXObject*,FXSelector,void* ptr){
  setText(*((FXString*)ptr));
  return 1;
  }


// Obtain value from text
long FXText::onCmdGetStringValue(FXObject*,FXSelector,void* ptr){
  getText(*((FXString*)ptr));
  return 1;
  }

/*******************************************************************************/

// Set tip using a message
long FXText::onCmdSetTip(FXObject*,FXSelector,void* ptr){
  setTipText(*((FXString*)ptr));
  return 1;
  }


// Get tip using a message
long FXText::onCmdGetTip(FXObject*,FXSelector,void* ptr){
  *((FXString*)ptr)=getTipText();
  return 1;
  }


// Set help using a message
long FXText::onCmdSetHelp(FXObject*,FXSelector,void* ptr){
  setHelpText(*((FXString*)ptr));
  return 1;
  }


// Get help using a message
long FXText::onCmdGetHelp(FXObject*,FXSelector,void* ptr){
  *((FXString*)ptr)=getHelpText();
  return 1;
  }


// We were asked about tip text
long FXText::onQueryTip(FXObject* sender,FXSelector sel,void* ptr){
  if(FXScrollArea::onQueryTip(sender,sel,ptr)) return 1;
  if((flags&FLAG_TIP) && !tip.empty()){
    sender->handle(this,FXSEL(SEL_COMMAND,ID_SETSTRINGVALUE),(void*)&tip);
    return 1;
    }
  return 0;
  }


// We were asked about status text
long FXText::onQueryHelp(FXObject* sender,FXSelector sel,void* ptr){
  if(FXScrollArea::onQueryHelp(sender,sel,ptr)) return 1;
  if((flags&FLAG_HELP) && !help.empty()){
    sender->handle(this,FXSEL(SEL_COMMAND,ID_SETSTRINGVALUE),(void*)&help);
    return 1;
    }
  return 0;
  }


// Update somebody who wants to change the text
long FXText::onUpdIsEditable(FXObject* sender,FXSelector,void*){
  sender->handle(this,isEditable()?FXSEL(SEL_COMMAND,ID_ENABLE):FXSEL(SEL_COMMAND,ID_DISABLE),NULL);
  return 1;
  }


// Update somebody who works on the selection
long FXText::onUpdHaveSelection(FXObject* sender,FXSelector,void*){
  sender->handle(this,(select.startpos<=select.endpos)?FXSEL(SEL_COMMAND,ID_ENABLE):FXSEL(SEL_COMMAND,ID_DISABLE),NULL);
  return 1;
  }


// Update somebody who works on the selection and change the text
long FXText::onUpdHaveEditableSelection(FXObject* sender,FXSelector,void*){
  sender->handle(this,isEditable() && (select.startpos<=select.endpos)?FXSEL(SEL_COMMAND,ID_ENABLE):FXSEL(SEL_COMMAND,ID_DISABLE),NULL);
  return 1;
  }


// Start input method editor
long FXText::onIMEStart(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(getComposeContext()){
      FXint th=font->getFontHeight();
      FXint cursory=getVisibleY()+margintop+pos_y+(cursorrow*th)+th;
      if(getVisibleY()<=cursory+th && cursory<=getVisibleY()+getVisibleHeight()){
        FXASSERT(0<=cursorrow-toprow && cursorrow-toprow<nvisrows);
        FXint cursorstart=visrows[cursorrow-toprow];
        FXint cursorx=getVisibleX()+marginleft+pos_x+xoffset(cursorstart,cursorpos)-1;
        getComposeContext()->setSpot(cursorx,cursory);
        }
      }
    return 1;
    }
  return 0;
  }

/*******************************************************************************/

// Start a drag operation
long FXText::onBeginDrag(FXObject* sender,FXSelector sel,void* ptr){
  FXDragType types[4]={stringType,textType,utf8Type,utf16Type};
  if(!FXScrollArea::onBeginDrag(sender,sel,ptr)){
    beginDrag(types,ARRAYNUMBER(types));
    setDragCursor(getApp()->getDefaultCursor(DEF_DNDSTOP_CURSOR));
    }
  return 1;
  }


// End drag operation
long FXText::onEndDrag(FXObject* sender,FXSelector sel,void* ptr){
  if(!FXScrollArea::onEndDrag(sender,sel,ptr)){
    endDrag((didAccept()!=DRAG_REJECT));
    setDragCursor(getApp()->getDefaultCursor(DEF_TEXT_CURSOR));
    }
  return 1;
  }


// Dragged stuff around
long FXText::onDragged(FXObject* sender,FXSelector sel,void* ptr){
  if(!FXScrollArea::onDragged(sender,sel,ptr)){
    FXDragAction action=DRAG_COPY;
    if(isEditable()){
      if(isDropTarget()) action=DRAG_MOVE;
      if(((FXEvent*)ptr)->state&CONTROLMASK) action=DRAG_COPY;
      if(((FXEvent*)ptr)->state&SHIFTMASK) action=DRAG_MOVE;
      }
    handleDrag(((FXEvent*)ptr)->root_x,((FXEvent*)ptr)->root_y,action);
    action=didAccept();
    switch(action){
      case DRAG_MOVE:
        setDragCursor(getApp()->getDefaultCursor(DEF_DNDMOVE_CURSOR));
        break;
      case DRAG_COPY:
        setDragCursor(getApp()->getDefaultCursor(DEF_DNDCOPY_CURSOR));
        break;
      default:
        setDragCursor(getApp()->getDefaultCursor(DEF_DNDSTOP_CURSOR));
        break;
      }
    }
  return 1;
  }


// Handle drag-and-drop enter
long FXText::onDNDEnter(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onDNDEnter(sender,sel,ptr);
  if(isEditable()){
    drawCursor(FLAG_CARET);
    }
  return 1;
  }


// Handle drag-and-drop leave
long FXText::onDNDLeave(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onDNDLeave(sender,sel,ptr);
  stopAutoScroll();
  if(isEditable()){
    drawCursor(0);
    }
  return 1;
  }


// Handle drag-and-drop motion
long FXText::onDNDMotion(FXObject* sender,FXSelector sel,void* ptr){
  FXEvent* event=(FXEvent*)ptr;

  // Scroll into view
  if(startAutoScroll(event,true)) return 1;

  // Handled elsewhere
  if(FXScrollArea::onDNDMotion(sender,sel,ptr)) return 1;

  // Correct drop type
  if(offeredDNDType(FROM_DRAGNDROP,textType) || offeredDNDType(FROM_DRAGNDROP,stringType) || offeredDNDType(FROM_DRAGNDROP,utf8Type) || offeredDNDType(FROM_DRAGNDROP,utf16Type)){

    // Is target editable?
    if(isEditable()){
      FXDragAction action=inquireDNDAction();

      // Check for legal DND action
      if(action==DRAG_COPY || action==DRAG_MOVE){
        FXint pos,row,col;

        // Get the suggested drop position
        pos=getRowColumnAt(event->win_x,event->win_y,row,col);

        // Move cursor to new position
        setCursorPos(pos,true);

        // We don't accept a drop on the selection
        if(!isPosSelected(pos,col)){
          acceptDrop(DRAG_ACCEPT);
          }
        }
      }
    return 1;
    }

  // Didn't handle it here
  return 0;
  }


// Handle drag-and-drop drop
long FXText::onDNDDrop(FXObject* sender,FXSelector sel,void* ptr){

  // Stop scrolling
  stopAutoScroll();
  drawCursor(0);

  // Try handling it in base class first
  if(FXScrollArea::onDNDDrop(sender,sel,ptr)) return 1;

  // Should really not have gotten this if non-editable
  if(isEditable()){
    FXString string;
    FXString junk;

    // First, try UTF-8
    if(getDNDData(FROM_DRAGNDROP,utf8Type,string)){
      if(inquireDNDAction()==DRAG_MOVE){
        getDNDData(FROM_DRAGNDROP,deleteType,junk);
        }
      replaceText(cursorpos,0,string,true);
      setCursorPos(cursorpos,true);
      return 1;
      }

    // Next, try UTF-16
    if(getDNDData(FROM_DRAGNDROP,utf16Type,string)){
      if(inquireDNDAction()==DRAG_MOVE){
        getDNDData(FROM_DRAGNDROP,deleteType,junk);
        }
      replaceText(cursorpos,0,string,true);
      setCursorPos(cursorpos,true);
      return 1;
      }

    // Next, try good old Latin-1
    if(getDNDData(FROM_DRAGNDROP,textType,string)){
      if(inquireDNDAction()==DRAG_MOVE){
        getDNDData(FROM_DRAGNDROP,deleteType,junk);
        }
      replaceText(cursorpos,0,string,true);
      setCursorPos(cursorpos,true);
      return 1;
      }
    return 1;
    }
  return 0;
  }


// Service requested DND data
long FXText::onDNDRequest(FXObject* sender,FXSelector sel,void* ptr){
  FXEvent *event=(FXEvent*)ptr;

  // Perhaps the target wants to supply its own data
  if(FXScrollArea::onDNDRequest(sender,sel,ptr)) return 1;

  // Recognize the request?
  if(event->target==stringType || event->target==textType || event->target==utf8Type || event->target==utf16Type){
    FXString string;

    // Get selected fragment
    string=getSelectedText();

    // Return text of the selection as UTF-8
    if(event->target==utf8Type){
      setDNDData(FROM_DRAGNDROP,event->target,string);
      return 1;
      }

    // Return text of the selection translated to 8859-1
    if(event->target==stringType || event->target==textType){
      setDNDData(FROM_DRAGNDROP,event->target,string);
      return 1;
      }

    // Return text of the selection translated to UTF-16
    if(event->target==utf16Type){
      setDNDData(FROM_DRAGNDROP,event->target,string);
      return 1;
      }
    }

  // Delete dragged text, if editable
  if(event->target==deleteType){
    if(isEditable()){
      if(select.startcol<=select.endcol){
        removeTextBlock(select.startpos,select.endpos,select.startcol,select.endcol,true);
        }
      else{
        removeText(select.startpos,select.endpos-select.startpos,true);
        }
      }
    return 1;
    }

  return 0;
  }

/*******************************************************************************/

// We now really do have the selection
long FXText::onSelectionGained(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onSelectionGained(sender,sel,ptr);
  return 1;
  }


// We lost the selection somehow
long FXText::onSelectionLost(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onSelectionLost(sender,sel,ptr);
  if(target){
    FXint what[4];
    what[0]=select.startpos;
    what[1]=select.endpos-select.startpos;
    what[2]=select.startcol;
    what[3]=select.endcol-select.startcol;
    target->tryHandle(this,FXSEL(SEL_DESELECTED,message),(void*)what);
    }
  updateRange(select.startpos,select.endpos);
  select.startpos=0;
  select.endpos=-1;
  select.startcol=0;
  select.endcol=-1;
  return 1;
  }


// Somebody wants our selection
long FXText::onSelectionRequest(FXObject* sender,FXSelector sel,void* ptr){
  FXEvent *event=(FXEvent*)ptr;

  // Perhaps the target wants to supply its own data for the selection
  if(FXScrollArea::onSelectionRequest(sender,sel,ptr)) return 1;

  // Recognize the request?
  if(event->target==stringType || event->target==textType || event->target==utf8Type || event->target==utf16Type){

    // Get selected fragment
    FXString string=getSelectedText();

    // Return text of the selection as UTF-8
    if(event->target==utf8Type){
      setDNDData(FROM_SELECTION,event->target,string);
      return 1;
      }

    // Return text of the selection translated to 8859-1
    if(event->target==stringType || event->target==textType){
      setDNDData(FROM_SELECTION,event->target,string);
      return 1;
      }

    // Return text of the selection translated to UTF-16
    if(event->target==utf16Type){
      setDNDData(FROM_SELECTION,event->target,string);
      return 1;
      }
    }
  return 0;
  }

/*******************************************************************************/

// We now really do have the selection
long FXText::onClipboardGained(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onClipboardGained(sender,sel,ptr);
  return 1;
  }


// We lost the selection somehow
long FXText::onClipboardLost(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onClipboardLost(sender,sel,ptr);
  clipped.clear();
  return 1;
  }


// Somebody wants our selection
long FXText::onClipboardRequest(FXObject* sender,FXSelector sel,void* ptr){
  FXEvent *event=(FXEvent*)ptr;
  FXString string=clipped;

  // Try handling it in base class first
  if(FXScrollArea::onClipboardRequest(sender,sel,ptr)) return 1;

  // Requested data from clipboard
  if(event->target==stringType || event->target==textType || event->target==utf8Type || event->target==utf16Type){

    // Expand newlines to CRLF on Windows
#ifdef WIN32
    unixToDos(string);
#endif

    // Return clipped text as as UTF-8
    if(event->target==utf8Type){
      setDNDData(FROM_CLIPBOARD,event->target,string);
      return 1;
      }

    // Return clipped text translated to 8859-1
    if(event->target==stringType || event->target==textType){
      setDNDData(FROM_CLIPBOARD,event->target,string);
      return 1;
      }

    // Return text of the selection translated to UTF-16
    if(event->target==utf16Type){
      setDNDData(FROM_CLIPBOARD,event->target,string);
      return 1;
      }
    }
  return 0;
  }

/*******************************************************************************/

// Pressed left button
long FXText::onLeftBtnPress(FXObject*,FXSelector,void* ptr){
  FXEvent* event=(FXEvent*)ptr;
  FXint pos,row,col;
  flags&=~FLAG_TIP;
  handle(this,FXSEL(SEL_FOCUS_SELF,0),ptr);
  if(isEnabled()){
    grab();
    if(target && target->tryHandle(this,FXSEL(SEL_LEFTBUTTONPRESS,message),ptr)) return 1;
    grabx=event->win_x-pos_x;
    graby=event->win_y-pos_y;
    if(event->click_count==1){
      pos=getRowColumnAt(event->win_x,event->win_y,row,col);
      if((event->state&CONTROLMASK) && !(options&TEXT_WORDWRAP)){
        if(event->state&SHIFTMASK){                     // Shift-select block
          moveCursorRowColumnAndSelect(row,col,true);
          }
        else{                                           // Drag select block
          moveCursorRowColumn(row,col,true);
          }
        mode=MOUSE_BLOCK;
        }
      else{
        if(event->state&SHIFTMASK){                     // Shift-select range
          moveCursorAndSelect(pos,SelectChars,true);
          }
        else{                                           // Drag select range
          moveCursor(pos,true);
          }
        mode=MOUSE_CHARS;
        }
      }
    else if(event->click_count==2){     // Drag select words
      pos=getPosContaining(event->win_x,event->win_y);
      setAnchorPos(pos);
      moveCursorAndSelect(pos,SelectWords,true);
      mode=MOUSE_WORDS;
      }
    else{                               // Drag select lines
      pos=getPosAt(event->win_x,event->win_y);
      moveCursorAndSelect(pos,SelectLines,true);
      mode=MOUSE_LINES;
      }
    flags&=~FLAG_UPDATE;
    return 1;
    }
  return 0;
  }


// Released left button
long FXText::onLeftBtnRelease(FXObject*,FXSelector,void* ptr){
  if(isEnabled()){
    ungrab();
    mode=MOUSE_NONE;
    stopAutoScroll();
    if(target && target->tryHandle(this,FXSEL(SEL_LEFTBUTTONRELEASE,message),ptr)) return 1;
    return 1;
    }
  return 0;
  }


// Pressed middle button
long FXText::onMiddleBtnPress(FXObject*,FXSelector,void* ptr){
  FXEvent* event=(FXEvent*)ptr;
  FXint pos,row,col;
  flags&=~FLAG_TIP;
  handle(this,FXSEL(SEL_FOCUS_SELF,0),ptr);
  if(isEnabled()){
    grab();
    if(target && target->tryHandle(this,FXSEL(SEL_MIDDLEBUTTONPRESS,message),ptr)) return 1;
    pos=getRowColumnAt(event->win_x,event->win_y,row,col);
    setCursorPos(pos,true);
    setAnchorPos(cursorpos);
    if(isPosSelected(cursorpos,col)){
      mode=MOUSE_TRYDRAG;
      }
    flags&=~FLAG_UPDATE;
    return 1;
    }
  return 0;
  }


// Released middle button
long FXText::onMiddleBtnRelease(FXObject*,FXSelector,void* ptr){
  FXuint md=mode;
  if(isEnabled()){
    ungrab();
    stopAutoScroll();
    mode=MOUSE_NONE;
    if(target && target->tryHandle(this,FXSEL(SEL_MIDDLEBUTTONRELEASE,message),ptr)) return 1;
    if(md==MOUSE_DRAG){
      handle(this,FXSEL(SEL_ENDDRAG,0),ptr);
      }
    else{
      handle(this,FXSEL(SEL_COMMAND,ID_PASTE_MIDDLE),NULL);
      }
    return 1;
    }
  return 0;
  }


// Pressed right button
long FXText::onRightBtnPress(FXObject*,FXSelector,void* ptr){
  FXEvent* event=(FXEvent*)ptr;
  flags&=~FLAG_TIP;
  handle(this,FXSEL(SEL_FOCUS_SELF,0),ptr);
  if(isEnabled()){
    grab();
    if(target && target->tryHandle(this,FXSEL(SEL_RIGHTBUTTONPRESS,message),ptr)) return 1;
    grabx=event->win_x-pos_x;
    graby=event->win_y-pos_y;
    mode=MOUSE_SCROLL;
    flags&=~FLAG_UPDATE;
    return 1;
    }
  return 0;
  }


// Released right button
long FXText::onRightBtnRelease(FXObject*,FXSelector,void* ptr){
  if(isEnabled()){
    ungrab();
    mode=MOUSE_NONE;
    if(target && target->tryHandle(this,FXSEL(SEL_RIGHTBUTTONRELEASE,message),ptr)) return 1;
    return 1;
    }
  return 0;
  }


// Handle real or simulated mouse motion
long FXText::onMotion(FXObject*,FXSelector,void* ptr){
  FXEvent* event=(FXEvent*)ptr;
  FXint pos,row,col;
  flags&=~FLAG_TIP;
  getApp()->removeTimeout(this,ID_TIPTIMER);
  switch(mode){
    case MOUSE_NONE:
      getApp()->addTimeout(this,ID_TIPTIMER,getApp()->getMenuPause());
      return 1;
    case MOUSE_CHARS:
      if(startAutoScroll(event,false)) return 1;
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        pos=getPosAt(event->win_x,event->win_y);
        setCursorPos(pos,true);
        extendSelection(cursorpos,SelectChars,true);
        }
      return 1;
    case MOUSE_WORDS:
      if(startAutoScroll(event,false)) return 1;
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        pos=getPosContaining(event->win_x,event->win_y);
        setCursorPos(pos,true);
        extendSelection(cursorpos,SelectWords,true);
        }
      return 1;
    case MOUSE_LINES:
      if(startAutoScroll(event,false)) return 1;
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        pos=getPosAt(event->win_x,event->win_y);
        setCursorPos(pos,true);
        extendSelection(cursorpos,SelectLines,true);
        }
      return 1;
    case MOUSE_BLOCK:
      if(startAutoScroll(event,false)) return 1;
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        getRowColumnAt(event->win_x,event->win_y,row,col);
        setCursorRowColumn(row,col,true);
        extendBlockSelection(row,col,true);
        }
      return 1;
    case MOUSE_SCROLL:
      setPosition(event->win_x-grabx,event->win_y-graby);
      return 1;
    case MOUSE_DRAG:
      handle(this,FXSEL(SEL_DRAGGED,0),ptr);
      return 1;
    case MOUSE_TRYDRAG:
      if(event->moved){
        mode=MOUSE_NONE;
        if(handle(this,FXSEL(SEL_BEGINDRAG,0),ptr)){
          mode=MOUSE_DRAG;
          }
        }
      return 1;
    }
  return 0;
  }


// Autoscroll timer fired; autoscrolling hysteresis is based on movement
// relative to the original document position of the click, in case the
// click-position is close to the autoscrolling fudge-border.
long FXText::onAutoScroll(FXObject* sender,FXSelector sel,void* ptr){
  FXEvent* event=(FXEvent*)ptr;
  FXint pos,row,col;
  FXScrollArea::onAutoScroll(sender,sel,ptr);
  switch(mode){
    case MOUSE_CHARS:
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        pos=getPosAt(event->win_x,event->win_y);
        extendSelection(pos,SelectChars,true);
        setCursorPos(pos,true);
        }
      return 1;
    case MOUSE_WORDS:
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        pos=getPosContaining(event->win_x,event->win_y);
        extendSelection(pos,SelectWords,true);
        setCursorPos(pos,true);
        }
      return 1;
    case MOUSE_LINES:
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        pos=getPosAt(event->win_x,event->win_y);
        extendSelection(pos,SelectLines,true);
        setCursorPos(pos,true);
        }
      return 1;
    case MOUSE_BLOCK:
      if((Math::iabs(event->win_x-grabx-pos_x)>getApp()->getDragDelta())||(Math::iabs(event->win_y-graby-pos_y)>getApp()->getDragDelta())){
        killHighlight();
        getRowColumnAt(event->win_x,event->win_y,row,col);
        extendBlockSelection(row,col,true);
        setCursorRowColumn(row,col,true);
        }
      return 1;
    }
  return 0;
  }


// The widget lost the grab for some reason
long FXText::onUngrabbed(FXObject* sender,FXSelector sel,void* ptr){
  FXScrollArea::onUngrabbed(sender,sel,ptr);
  mode=MOUSE_NONE;
  flags|=FLAG_UPDATE;
  stopAutoScroll();
  return 1;
  }


// Mouse hovered a while
long FXText::onTipTimer(FXObject*,FXSelector,void*){
  FXTRACE((250,"%s::onTipTimer %p\n",getClassName(),this));
  flags|=FLAG_TIP;
  return 1;
  }

/*******************************************************************************/

// Keyboard press
long FXText::onKeyPress(FXObject*,FXSelector,void* ptr){
  flags&=~FLAG_TIP;
  if(isEnabled()){
    FXEvent* event=(FXEvent*)ptr;
    FXTRACE((200,"%s::onKeyPress keysym=0x%04x state=%04x\n",getClassName(),event->code,event->state));
    if(target && target->tryHandle(this,FXSEL(SEL_KEYPRESS,message),ptr)) return 1;
    flags&=~FLAG_UPDATE;
    switch(event->code){
      case KEY_Shift_L:
      case KEY_Shift_R:
      case KEY_Control_L:
      case KEY_Control_R:
        if(mode==MOUSE_DRAG){handle(this,FXSEL(SEL_DRAGGED,0),ptr);}
        return 1;
      case KEY_Up:
      case KEY_KP_Up:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_SCROLL_UP),NULL);
          }
        else if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_UP),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_UP),NULL);
          }
        break;
      case KEY_Down:
      case KEY_KP_Down:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_SCROLL_DOWN),NULL);
          }
        else if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_DOWN),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_DOWN),NULL);
          }
        break;
      case KEY_Left:
      case KEY_KP_Left:
        if(event->state&CONTROLMASK){
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_WORD_LEFT),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_WORD_LEFT),NULL);
            }
          }
        else{
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_LEFT),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_LEFT),NULL);
            }
          }
        break;
      case KEY_Right:
      case KEY_KP_Right:
        if(event->state&CONTROLMASK){
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_WORD_RIGHT),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_WORD_RIGHT),NULL);
            }
          }
        else{
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_RIGHT),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_RIGHT),NULL);
            }
          }
        break;
      case KEY_Home:
      case KEY_KP_Home:
        if(event->state&CONTROLMASK){
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_TOP),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_TOP),NULL);
            }
          }
        else{
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_HOME),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_HOME),NULL);
            }
          }
        break;
      case KEY_End:
      case KEY_KP_End:
        if(event->state&CONTROLMASK){
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_BOTTOM),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_BOTTOM),NULL);
            }
          }
        else{
          if(event->state&SHIFTMASK){
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_END),NULL);
            }
          else{
            handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_END),NULL);
            }
          }
        break;
      case KEY_Page_Up:
      case KEY_KP_Page_Up:
        if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_PAGEUP),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_PAGEUP),NULL);
          }
        break;
      case KEY_Page_Down:
      case KEY_KP_Page_Down:
        if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_SHIFT_PAGEDOWN),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_CURSOR_PAGEDOWN),NULL);
          }
        break;
      case KEY_Insert:
      case KEY_KP_Insert:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_COPY_SEL),NULL);
          }
        else if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_PASTE_SEL),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_TOGGLE_OVERSTRIKE),NULL);
          }
        break;
      case KEY_Delete:
      case KEY_KP_Delete:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_DELETE_WORD),NULL);
          }
        else if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_DELETE_EOL),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_DELETE_CHAR),NULL);
          }
        break;
      case KEY_BackSpace:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_BACKSPACE_WORD),NULL);
          }
        else if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_BACKSPACE_BOL),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_BACKSPACE_CHAR),NULL);
          }
        break;
      case KEY_Return:
      case KEY_KP_Enter:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_INSERT_NEWLINE_ONLY),NULL);
          }
        else if(event->state&SHIFTMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_INSERT_NEWLINE_INDENT),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_INSERT_NEWLINE),NULL);
          }
        break;
      case KEY_Tab:
      case KEY_KP_Tab:
        if(event->state&CONTROLMASK){
          handle(this,FXSEL(SEL_COMMAND,ID_INSERT_HARDTAB),NULL);
          }
        else{
          handle(this,FXSEL(SEL_COMMAND,ID_INSERT_TAB),NULL);
          }
        break;
      case KEY_a:
        if(!(event->state&CONTROLMASK)) goto ins;
        handle(this,FXSEL(SEL_COMMAND,ID_SELECT_ALL),NULL);
        break;
      case KEY_x:
        if(!(event->state&CONTROLMASK)) goto ins;
      case KEY_F20:                               // Sun Cut key
        handle(this,FXSEL(SEL_COMMAND,ID_CUT_SEL),NULL);
        break;
      case KEY_c:
        if(!(event->state&CONTROLMASK)) goto ins;
      case KEY_F16:                               // Sun Copy key
        handle(this,FXSEL(SEL_COMMAND,ID_COPY_SEL),NULL);
        break;
      case KEY_v:
        if(!(event->state&CONTROLMASK)) goto ins;
      case KEY_F18:                               // Sun Paste key
        handle(this,FXSEL(SEL_COMMAND,ID_PASTE_SEL),NULL);
        break;
      case KEY_k:
        if(!(event->state&CONTROLMASK)) goto ins;
        handle(this,FXSEL(SEL_COMMAND,ID_DELETE_LINE),NULL);
        break;
      case KEY_j:
        if(!(event->state&CONTROLMASK)) goto ins;
        handle(this,FXSEL(SEL_COMMAND,ID_JOIN_LINES),NULL);
        break;
      default:
ins:    if((event->state&(CONTROLMASK|ALTMASK)) || ((FXuchar)event->text[0]<32)) return 0;
        handle(this,FXSEL(SEL_COMMAND,ID_INSERT_STRING),(void*)event->text.text());
        break;
      }
    return 1;
    }
  return 0;
  }


// Keyboard release
long FXText::onKeyRelease(FXObject*,FXSelector,void* ptr){
  if(isEnabled()){
    FXEvent* event=(FXEvent*)ptr;
    FXTRACE((200,"%s::onKeyRelease keysym=0x%04x state=%04x\n",getClassName(),event->code,event->state));
    if(target && target->tryHandle(this,FXSEL(SEL_KEYRELEASE,message),ptr)) return 1;
    switch(event->code){
      case KEY_Shift_L:
      case KEY_Shift_R:
      case KEY_Control_L:
      case KEY_Control_R:
        if(mode==MOUSE_DRAG){handle(this,FXSEL(SEL_DRAGGED,0),ptr);}
        return 1;
      }
    }
  return 0;
  }

/*******************************************************************************/

// Move cursor to top of buffer
long FXText::onCmdCursorTop(FXObject*,FXSelector,void*){
  moveCursor(0,true);
  return 1;
  }


// Move cursor to bottom of buffer
long FXText::onCmdCursorBottom(FXObject*,FXSelector,void*){
  moveCursor(length,true);
  return 1;
  }


// Move cursor to begin of line
long FXText::onCmdCursorHome(FXObject*,FXSelector,void*){
  moveCursor(lineStart(cursorpos),true);
  return 1;
  }


// Move cursor to end of line
long FXText::onCmdCursorEnd(FXObject*,FXSelector,void*){
  moveCursor(lineEnd(cursorpos),true);
  return 1;
  }


// Process cursor right
long FXText::onCmdCursorRight(FXObject*,FXSelector,void*){
  moveCursor(cursorpos<length?inc(cursorpos):length,true);
  return 1;
  }


// Process cursor left
long FXText::onCmdCursorLeft(FXObject*,FXSelector,void*){
  moveCursor(0<cursorpos?dec(cursorpos):0,true);
  return 1;
  }


// Process cursor up
long FXText::onCmdCursorUp(FXObject*,FXSelector,void*){
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  moveCursor(posFromColumn(prevRow(cursorpos),col),true);
  prefcol=col;
  return 1;
  }


// Process cursor down
long FXText::onCmdCursorDown(FXObject*,FXSelector,void*){
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  moveCursor(posFromColumn(nextRow(cursorpos),col),true);
  prefcol=col;
  return 1;
  }


// Page up
long FXText::onCmdCursorPageUp(FXObject*,FXSelector,void*){
  FXint lines=getVisibleHeight()/font->getFontHeight();
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  setTopLine(prevRow(toppos,lines));
  moveCursor(posFromColumn(prevRow(cursorpos,lines),col),true);
  prefcol=col;
  return 1;
  }


// Page down
long FXText::onCmdCursorPageDown(FXObject*,FXSelector,void*){
  FXint lines=getVisibleHeight()/font->getFontHeight();
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  setTopLine(nextRow(toppos,lines));
  moveCursor(posFromColumn(nextRow(cursorpos,lines),col),true);
  prefcol=col;
  return 1;
  }


// Process cursor word left
long FXText::onCmdCursorWordLeft(FXObject*,FXSelector,void*){
  moveCursor(leftWord(cursorpos),true);
  return 1;
  }


// Process cursor word right
long FXText::onCmdCursorWordRight(FXObject*,FXSelector,void*){
  moveCursor(rightWord(cursorpos),true);
  return 1;
  }


// Process cursor shift+top
long FXText::onCmdCursorShiftTop(FXObject*,FXSelector,void*){
  moveCursorAndSelect(0,SelectChars,true);
  return 1;
  }


// Process cursor shift+bottom
long FXText::onCmdCursorShiftBottom(FXObject*,FXSelector,void*){
  moveCursorAndSelect(length,SelectChars,true);
  return 1;
  }


// Process cursor shift+home
long FXText::onCmdCursorShiftHome(FXObject*,FXSelector,void*){
  moveCursorAndSelect(lineStart(cursorpos),SelectChars,true);
  return 1;
  }


// Process cursor shift+end
long FXText::onCmdCursorShiftEnd(FXObject*,FXSelector,void*){
  moveCursorAndSelect(lineEnd(cursorpos),SelectChars,true);
  return 1;
  }


// Process cursor shift+right
long FXText::onCmdCursorShiftRight(FXObject*,FXSelector,void*){
  moveCursorAndSelect(cursorpos<length?inc(cursorpos):length,SelectChars,true);
  return 1;
  }


// Process cursor shift+left
long FXText::onCmdCursorShiftLeft(FXObject*,FXSelector,void*){
  moveCursorAndSelect(0<cursorpos?dec(cursorpos):0,SelectChars,true);
  return 1;
  }


// Process cursor shift+up
long FXText::onCmdCursorShiftUp(FXObject*,FXSelector,void*){
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  moveCursorAndSelect(posFromColumn(prevRow(cursorpos),col),SelectChars,true);
  prefcol=col;
  return 1;
  }


// Process cursor shift+down
long FXText::onCmdCursorShiftDown(FXObject*,FXSelector,void*){
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  moveCursorAndSelect(posFromColumn(nextRow(cursorpos),col),SelectChars,true);
  prefcol=col;
  return 1;
  }


// Process cursor shift+page up
long FXText::onCmdCursorShiftPageUp(FXObject*,FXSelector,void*){
  FXint lines=getVisibleHeight()/font->getFontHeight();
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  setTopLine(prevRow(toppos,lines));
  moveCursorAndSelect(posFromColumn(prevRow(cursorpos,lines),col),SelectChars,true);
  prefcol=col;
  return 1;
  }


// Process cursor shift+page down
long FXText::onCmdCursorShiftPageDown(FXObject*,FXSelector,void*){
  FXint lines=getVisibleHeight()/font->getFontHeight();
  FXint col=(0<=prefcol) ? prefcol : cursorcol;
  setTopLine(nextRow(toppos,lines));
  moveCursorAndSelect(posFromColumn(nextRow(cursorpos,lines),col),SelectChars,true);
  prefcol=col;
  return 1;
  }


// Process cursor shift+word left
long FXText::onCmdCursorShiftWordLeft(FXObject*,FXSelector,void*){
  moveCursorAndSelect(leftWord(cursorpos),SelectChars,true);
  return 1;
  }


// Process cursor shift+word right
long FXText::onCmdCursorShiftWordRight(FXObject*,FXSelector,void*){
  moveCursorAndSelect(rightWord(cursorpos),SelectChars,true);
  return 1;
  }


// Scroll up one line
long FXText::onCmdScrollUp(FXObject*,FXSelector,void*){
  setTopLine(prevRow(toppos));
  return 1;
  }


// Scroll down one line
long FXText::onCmdScrollDown(FXObject*,FXSelector,void*){
  setTopLine(nextRow(toppos));
  return 1;
  }


// Scroll to move cursor to top of screen
long FXText::onCmdScrollTop(FXObject*,FXSelector,void*){
  setTopLine(cursorpos);
  return 1;
  }


// Scroll to move cursor to bottom of screen
long FXText::onCmdScrollBottom(FXObject*,FXSelector,void*){
  setBottomLine(cursorpos);
  return 1;
  }


// Scroll to move cursor to center of screen
long FXText::onCmdScrollCenter(FXObject*,FXSelector,void*){
  setCenterLine(cursorpos);
  return 1;
  }


// Insert a string
long FXText::onCmdInsertString(FXObject*,FXSelector,void* ptr){
  if(isEditable()){
    FXchar* txt=(FXchar*)ptr;
    FXint len=strlen(txt);
    FXint beg=cursorpos;
    FXint end=cursorpos;
    FXint ins,cols;
    if(isPosSelected(cursorpos,cursorvcol)){
      beg=select.startpos;
      end=select.endpos;
      if(select.startcol<=select.endcol){
        cols=maxcolumns(txt,txt+len,tabcolumns);
//      m=replaceTextBlock(select.startpos,select.endpos,select.startcol,select.endcol,text,n,notify);
        ins=insertTextBlock(beg,end,select.startcol,txt,len,true);
        select.startcol=select.endcol=select.startcol+cols;
        beg=posFromColumn(lineStart(beg+ins),select.startcol);
        setCursorPos(beg,true);
        return 1;
        }
      ins=replaceText(beg,end-beg,txt,len,true);
      moveCursor(beg+ins,true);
      return 1;
      }
    if(isOverstrike()) end=overstruck(beg,end,txt,len);
    ins=replaceText(beg,end-beg,txt,len,true);
    moveCursor(beg+ins,true);
    return 1;
    }
  getApp()->beep();
  return 1;
  }


// Insert newline with optional autoindent
long FXText::onCmdInsertNewline(FXObject*,FXSelector,void*){
  return handle(this,(options&TEXT_AUTOINDENT)?FXSEL(SEL_COMMAND,ID_INSERT_NEWLINE_INDENT):FXSEL(SEL_COMMAND,ID_INSERT_NEWLINE_ONLY),NULL);
  }


// Insert newline only
long FXText::onCmdInsertNewlineOnly(FXObject*,FXSelector,void*){
  return handle(this,FXSEL(SEL_COMMAND,ID_INSERT_STRING),(void*)"\n");
  }


// Insert a character
long FXText::onCmdInsertNewlineIndent(FXObject*,FXSelector,void*){
  FXint pos=isPosSelected(cursorpos) ? select.startpos : cursorpos;
  FXint start=lineStart(pos);
  FXString string=extractText(start,pos-start);
  FXint n=string.find_first_not_of(" \t\v");
  if(0<=n) string.trunc(n);
  string.prepend('\n');
  return handle(this,FXSEL(SEL_COMMAND,ID_INSERT_STRING),(void*)string.text());
  }


// Insert optional soft-tab
long FXText::onCmdInsertTab(FXObject*,FXSelector,void*){
  return handle(this,(options&TEXT_NO_TABS)?FXSEL(SEL_COMMAND,ID_INSERT_SOFTTAB):FXSEL(SEL_COMMAND,ID_INSERT_HARDTAB),NULL);
  }


// Insert hard-tab
long FXText::onCmdInsertHardTab(FXObject*,FXSelector,void*){
  return handle(this,FXSEL(SEL_COMMAND,ID_INSERT_STRING),(void*)"\t");
  }


// Insert soft-tab
long FXText::onCmdInsertSoftTab(FXObject*,FXSelector,void*){
  FXint pos=isPosSelected(cursorpos) ? select.startpos : cursorpos;
  FXint indent=columnFromPos(lineStart(pos),pos);
  FXASSERT(0<tabcolumns && tabcolumns<MAXTABCOLUMNS);
  return handle(this,FXSEL(SEL_COMMAND,ID_INSERT_STRING),(void*)(spaces+MAXTABCOLUMNS+indent%tabcolumns-tabcolumns));
  }

/*******************************************************************************/

// Cut
long FXText::onCmdCutSel(FXObject*,FXSelector,void*){
  if(isEditable() && cutSelection(true)) return 1;
  getApp()->beep();
  return 1;
  }


// Copy
long FXText::onCmdCopySel(FXObject*,FXSelector,void*){
  copySelection();
  return 1;
  }


// Paste clipboard
long FXText::onCmdPasteSel(FXObject*,FXSelector,void*){
  if(isEditable() && pasteClipboard(true)) return 1;
  getApp()->beep();
  return 1;
  }


// Paste selection
long FXText::onCmdPasteMiddle(FXObject*,FXSelector,void*){
  if(isEditable() && pasteSelection(true)) return 1;
  getApp()->beep();
  return 1;
  }


// Delete selection
long FXText::onCmdDeleteSel(FXObject*,FXSelector,void*){
  if(isEditable() && deleteSelection(true)) return 1;
  getApp()->beep();
  return 1;
  }


// Replace selection
long FXText::onCmdReplaceSel(FXObject*,FXSelector,void* ptr){
  if(isEditable() && replaceSelection((const FXchar*)ptr,true)) return 1;
  getApp()->beep();
  return 1;
  }


// Select character
long FXText::onCmdSelectChar(FXObject*,FXSelector,void*){
  setAnchorPos(cursorpos);
  extendSelection(inc(cursorpos),SelectChars,true);
  return 1;
  }


// Select Word
long FXText::onCmdSelectWord(FXObject*,FXSelector,void*){
  setAnchorPos(cursorpos);
  extendSelection(cursorpos,SelectWords,true);
  return 1;
  }


// Select Line
long FXText::onCmdSelectLine(FXObject*,FXSelector,void*){
  setAnchorPos(cursorpos);
  extendSelection(cursorpos,SelectLines,true);
  return 1;
  }


// Select text till matching character
long FXText::onCmdSelectMatching(FXObject*,FXSelector,void*){
  if(0<cursorpos){
    FXchar ch=getByte(cursorpos-1);
    FXint pos=findMatching(cursorpos-1,0,length,ch,1);
    if(0<=pos){
      if(cursorpos<=pos){
        setSelection(cursorpos-1,pos-cursorpos+2,true);
        setAnchorPos(cursorpos-1);
        setCursorPos(pos+1,true);
        }
      else{
        setSelection(pos,cursorpos-pos,true);
        setAnchorPos(cursorpos);
        setCursorPos(pos+1,true);
        }
      makePositionVisible(cursorpos);
      flashMatching();
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Select entire enclosing block
long FXText::onCmdSelectBlock(FXObject*,FXSelector sel,void*){
  FXint what=FXSELID(sel)-ID_SELECT_BRACE;
  FXint level=1;
  FXint beg,end;
  while(1){
    beg=matchBackward(cursorpos-1,0,lefthand[what],righthand[what],level);
    end=matchForward(cursorpos,length,lefthand[what],righthand[what],level);
    if(0<=beg && beg<end){
      if(isPosSelected(beg) && isPosSelected(end+1)){ level++; continue; }
      setAnchorPos(beg);
      extendSelection(end+1,SelectChars,true);
      return 1;
      }
    getApp()->beep();
    break;
    }
  return 1;
  }


// Select All
long FXText::onCmdSelectAll(FXObject*,FXSelector,void*){
  setAnchorPos(0);
  extendSelection(length,SelectChars,true);
  return 1;
  }


// Deselect All
long FXText::onCmdDeselectAll(FXObject*,FXSelector,void*){
  killSelection(true);
  return 1;
  }

/*******************************************************************************/

// Backspace character
long FXText::onCmdBackspaceChar(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(deletePendingSelection(true)) return 1;
    if(0<cursorpos){
      FXint pos=dec(cursorpos);
      removeText(pos,cursorpos-pos,true);
      moveCursor(pos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Backspace word
long FXText::onCmdBackspaceWord(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(deletePendingSelection(true)) return 1;
    FXint pos=leftWord(cursorpos);
    if(pos<cursorpos){
      removeText(pos,cursorpos-pos,true);
      moveCursor(pos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Backspace bol
long FXText::onCmdBackspaceBol(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(deletePendingSelection(true)) return 1;
    FXint pos=lineStart(cursorpos);
    if(pos<cursorpos){
      removeText(pos,cursorpos-pos,true);
      moveCursor(pos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Delete character
long FXText::onCmdDeleteChar(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(deletePendingSelection(true)) return 1;
    if(cursorpos<length){
      FXint pos=inc(cursorpos);
      removeText(cursorpos,pos-cursorpos,true);
      moveCursor(cursorpos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Delete word
long FXText::onCmdDeleteWord(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(deletePendingSelection(true)) return 1;
    FXint pos=rightWord(cursorpos);
    if(pos<length){
      removeText(cursorpos,pos-cursorpos,true);
      moveCursor(cursorpos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Delete to end of line
long FXText::onCmdDeleteEol(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(deletePendingSelection(true)) return 1;
    FXint pos=lineEnd(cursorpos);
    if(pos<length){
      removeText(cursorpos,pos-cursorpos,true);
      moveCursor(cursorpos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Delete line
long FXText::onCmdDeleteLine(FXObject*,FXSelector,void*){
  if(isEditable()){
    FXint beg=lineStart(cursorpos);
    FXint end=nextLine(cursorpos);
    if(beg<end){
      removeText(beg,end-beg,true);
      moveCursor(beg,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Delete all text
long FXText::onCmdDeleteAll(FXObject*,FXSelector,void*){
  if(isEditable()){
    if(0<length){
      removeText(0,length,true);
      moveCursor(0,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }

/*******************************************************************************/

// Shift selected lines left or right, or clean indent
// Try keep the cursor on same row and (adjusted) column as before
long FXText::onCmdShiftText(FXObject*,FXSelector sel,void*){
  if(isEditable()){
    FXint startpos,endpos,len;
    FXint curc=getCursorColumn();
    FXint curr=getCursorRow();
    FXint indent=0;
    switch(FXSELID(sel)){
      case ID_SHIFT_LEFT: indent=-1; break;
      case ID_SHIFT_RIGHT: indent=1; break;
      case ID_SHIFT_TABLEFT: indent=-tabcolumns; break;
      case ID_SHIFT_TABRIGHT: indent=tabcolumns; break;
      }
    if(select.startpos<=select.endpos){
      startpos=lineStart(select.startpos);
      endpos=nextLine(select.endpos-1);
      }
    else{
      startpos=lineStart(cursorpos);
      endpos=lineEnd(cursorpos);
      if(endpos<length) endpos++;
      }
    len=shiftText(startpos,endpos,indent,true);
    setSelection(startpos,len,true);
    setAnchorRowColumn(curr,FXMAX(curc+indent,0));
    setCursorRowColumn(curr,FXMAX(curc+indent,0),true);
    }
  else{
    getApp()->beep();
    }
  return 1;
  }

/*******************************************************************************/

// Make selected text upper case
long FXText::onCmdChangeCase(FXObject*,FXSelector sel,void*){
  if(isEditable()){
    FXint startpos,endpos,len;
    FXint upper=(FXSELID(sel)==ID_UPPER_CASE);
    FXint curc=getCursorColumn();
    FXint curr=getCursorRow();
    if(select.startpos<=select.endpos){
      startpos=select.startpos;
      endpos=select.endpos;
      }
    else{
      startpos=cursorpos;
      endpos=inc(cursorpos);
      }
    len=caseShift(startpos,endpos,upper,true);
    setSelection(startpos,len,true);
    setAnchorRowColumn(curr,curc);
    setCursorRowColumn(curr,curc,true);
    }
  else{
    getApp()->beep();
    }
  return 1;
  }

/*******************************************************************************/

// Copy current line to the line below; leave it selected with cursor at the end
long FXText::onCmdCopyLine(FXObject*,FXSelector,void*){
  if(isEditable()){
    FXString text;
    FXint start,end;
    FXASSERT(0<=select.startpos);
    FXASSERT(select.startpos<=select.endpos);
    FXASSERT(select.endpos<=length);
    if(select.startpos<=select.endpos){
      start=lineStart(select.startpos);
      end=lineEnd(select.endpos-1);
      }
    else{
      start=lineStart(cursorpos);
      end=lineEnd(cursorpos);
      }
    text=extractText(start,end-start);
    text+='\n';
    insertText(start,text,true);
    setSelection(start+text.length(),text.length(),true);
    setAnchorPos(cursorpos);
    makePositionVisible(cursorpos);
    return 1;
    }
  getApp()->beep();
  return 1;
  }

/*******************************************************************************/

// Move the current line up, if there is a line above it.
// More tricky than it looks; current line may be non-terminated by a newline.
// However, previous line *is* newline terminated by definition.
// Solution is to snip the lines without the newline, and then place the
// newline at the appropriate spot.
long FXText::onCmdMoveLineUp(FXObject*,FXSelector,void*){
  if(isEditable()){
    FXint curbeg,curend,prvbeg,pos;
    FXASSERT(0<=select.startpos);
    FXASSERT(select.startpos<=select.endpos);
    FXASSERT(select.endpos<=length);
    if(select.startpos<=select.endpos){
      curbeg=lineStart(select.startpos);
      curend=lineEnd(select.endpos-1);
      }
    else{
      curbeg=lineStart(cursorpos);
      curend=lineEnd(cursorpos);
      }
    FXASSERT(curbeg<=curend);
    prvbeg=prevLine(curbeg);
    if(0<curbeg){
      FXString text('\0',curend-prvbeg);
      pos=prvbeg+cursorpos-curbeg;
      extractText(&text[0],curbeg,curend-curbeg);
      text[curend-curbeg]='\n';
      extractText(&text[curend-curbeg+1],prvbeg,curbeg-prvbeg-1);
      replaceText(prvbeg,curend-prvbeg,text,true);
      setSelection(prvbeg,curend-curbeg+1,true);
      setAnchorPos(prvbeg);
      setCursorPos(pos,true);
      makePositionVisible(cursorpos);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Move current line down, if there is a line below it.
// Similar logic as above; the line to be moved up may be non-terminated by a newline.
// The current line *is* newline terminated, by definition.
// Thus we snip the lines w/o including the newline, and place the missing
// newline at the proper place in the middle.
long FXText::onCmdMoveLineDown(FXObject*,FXSelector,void*){
  if(isEditable()){
    FXint curbeg,curend,nxtend,pos;
    FXASSERT(0<=select.startpos);
    FXASSERT(select.startpos<=select.endpos);
    FXASSERT(select.endpos<=length);
    if(select.startpos<=select.endpos){
      curbeg=lineStart(select.startpos);
      curend=nextLine(select.endpos-1);
      }
    else{
      curbeg=lineStart(cursorpos);
      curend=nextLine(cursorpos);
      }
    nxtend=lineEnd(curend);
    if(curend<length){
      FXString text('\0',nxtend-curbeg);
      pos=nxtend-curend+cursorpos;
      extractText(&text[0],curend,nxtend-curend);
      text[nxtend-curend]='\n';
      extractText(&text[nxtend-curend+1],curbeg,curend-curbeg-1);
      replaceText(curbeg,nxtend-curbeg,text,true);
      setSelection(curbeg+nxtend-curend+1,curend-curbeg,true);
      setAnchorPos(curbeg+nxtend-curend+1);
      setCursorPos(pos,true);
      makePositionVisible(cursorpos);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }

/*******************************************************************************/

// Join lines
long FXText::onCmdJoinLines(FXObject*,FXSelector,void*){
  if(isEditable()){
    FXint pos=lineEnd(cursorpos);
    if(pos<length){
      removeText(pos,1,true);
      return 1;
      }
    }
  else{
    getApp()->beep();
    }
  return 1;
  }


// Goto start of enclosing block
long FXText::onCmdBlockBeg(FXObject*,FXSelector sel,void*){
  FXint what=FXSELID(sel)-ID_LEFT_BRACE;
  FXint beg=cursorpos-1;
  if(0<beg){
    if(getByte(beg)==lefthand[what]) beg--;
    FXint pos=matchBackward(beg,0,lefthand[what],righthand[what],1);
    if(0<=pos){
      moveCursor(pos+1,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Goto end of enclosing block
long FXText::onCmdBlockEnd(FXObject*,FXSelector sel,void*){
  FXint what=FXSELID(sel)-ID_RIGHT_BRACE;
  FXint start=cursorpos;
  if(start<length){
    if(getByte(start)==righthand[what]) start++;
    FXint pos=matchForward(start,length,lefthand[what],righthand[what],1);
    if(0<=pos){
      moveCursor(pos,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Goto matching character
long FXText::onCmdGotoMatching(FXObject*,FXSelector,void*){
  if(0<cursorpos){
    FXchar ch=getByte(cursorpos-1);
    FXint pos=findMatching(cursorpos-1,0,length,ch,1);
    if(0<=pos){
      moveCursor(pos+1,true);
      return 1;
      }
    }
  getApp()->beep();
  return 1;
  }


// Move cursor to indicated row
long FXText::onCmdCursorRow(FXObject* sender,FXSelector,void*){
  FXint row=cursorrow+1;
  sender->handle(this,FXSEL(SEL_COMMAND,ID_GETINTVALUE),(void*)&row);
  setCursorRow(row-1,true);
  return 1;
  }


// Being asked about current row number
long FXText::onUpdCursorRow(FXObject* sender,FXSelector,void*){
  FXint row=cursorrow+1;
  sender->handle(this,FXSEL(SEL_COMMAND,ID_SETINTVALUE),(void*)&row);
  return 1;
  }


// Move cursor to indicated column
long FXText::onCmdCursorColumn(FXObject* sender,FXSelector,void*){
  FXint col=cursorcol;
  sender->handle(this,FXSEL(SEL_COMMAND,ID_GETINTVALUE),(void*)&col);
  setCursorColumn(col,true);
  return 1;
  }


// Being asked about current column
long FXText::onUpdCursorColumn(FXObject* sender,FXSelector,void*){
  sender->handle(this,FXSEL(SEL_COMMAND,FXWindow::ID_SETINTVALUE),(void*)&cursorcol);
  return 1;
  }


// Editable toggle
long FXText::onCmdToggleEditable(FXObject*,FXSelector,void*){
  setEditable(!isEditable());
  return 1;
  }


// Update editable toggle
long FXText::onUpdToggleEditable(FXObject* sender,FXSelector,void*){
  sender->handle(this,isEditable()?FXSEL(SEL_COMMAND,ID_CHECK):FXSEL(SEL_COMMAND,ID_UNCHECK),NULL);
  sender->handle(this,FXSEL(SEL_COMMAND,ID_SHOW),NULL);
  sender->handle(this,FXSEL(SEL_COMMAND,ID_ENABLE),NULL);
  return 1;
  }


// Overstrike toggle
long FXText::onCmdToggleOverstrike(FXObject*,FXSelector,void*){
  setOverstrike(!isOverstrike());
  return 1;
  }


// Update overstrike toggle
long FXText::onUpdToggleOverstrike(FXObject* sender,FXSelector,void*){
  sender->handle(this,isOverstrike()?FXSEL(SEL_COMMAND,ID_CHECK):FXSEL(SEL_COMMAND,ID_UNCHECK),NULL);
  sender->handle(this,FXSEL(SEL_COMMAND,ID_SHOW),NULL);
  sender->handle(this,FXSEL(SEL_COMMAND,ID_ENABLE),NULL);
  return 1;
  }

/*******************************************************************************/

// Change top margin
void FXText::setMarginTop(FXint mt){
  if(margintop!=mt){
    margintop=mt;
    recalc();
    update();
    }
  }


// Change bottom margin
void FXText::setMarginBottom(FXint mb){
  if(marginbottom!=mb){
    marginbottom=mb;
    recalc();
    update();
    }
  }


// Change left margin
void FXText::setMarginLeft(FXint ml){
  if(marginleft!=ml){
    marginleft=ml;
    recalc();
    update();
    }
  }


// Change right margin
void FXText::setMarginRight(FXint mr){
  if(marginright!=mr){
    marginright=mr;
    recalc();
    update();
    }
  }


// Change the font
void FXText::setFont(FXFont* fnt){
  if(!fnt){ fxerror("%s::setFont: NULL font specified.\n",getClassName()); }
  if(font!=fnt){
    font=fnt;
    tabwidth=tabcolumns*font->getTextWidth(" ",1);
    barwidth=barcolumns*font->getTextWidth("8",1);
    if(getComposeContext()) getComposeContext()->setFont(font);
    recalc();
    update();
    }
  }


// Set wrap columns
void FXText::setWrapColumns(FXint cols){
  if(cols<=0) cols=1;
  if(cols!=wrapcolumns){
    wrapcolumns=cols;
    recalc();
    update();
    }
  }


// Set tab columns
void FXText::setTabColumns(FXint cols){
  cols=FXCLAMP(1,cols,MAXTABCOLUMNS);
  if(cols!=tabcolumns){
    tabcolumns=cols;
    tabwidth=tabcolumns*font->getTextWidth(" ",1);
    recalc();
    update();
    }
  }


// Change number of columns used for line numbers
void FXText::setBarColumns(FXint cols){
  if(cols<=0) cols=0;
  if(cols!=barcolumns){
    barcolumns=cols;
    barwidth=barcolumns*font->getTextWidth("8",1);
    recalc();
    update();
    }
  }


// Set text color
void FXText::setTextColor(FXColor clr){
  if(clr!=textColor){
    textColor=clr;
    update();
    }
  }


// Set select background color
void FXText::setSelBackColor(FXColor clr){
  if(clr!=selbackColor){
    selbackColor=clr;
    update();
    }
  }


// Set selected text color
void FXText::setSelTextColor(FXColor clr){
  if(clr!=seltextColor){
    seltextColor=clr;
    update();
    }
  }


// Change highlighted text color
void FXText::setHiliteTextColor(FXColor clr){
  if(clr!=hilitetextColor){
    hilitetextColor=clr;
    update();
    }
  }


// Change highlighted background color
void FXText::setHiliteBackColor(FXColor clr){
  if(clr!=hilitebackColor){
    hilitebackColor=clr;
    update();
    }
  }


// Change active background color
void FXText::setActiveBackColor(FXColor clr){
  if(clr!=activebackColor){
    activebackColor=clr;
    update();
    }
  }

// Change line number color
void FXText::setNumberColor(FXColor clr){
  if(clr!=numberColor){
    numberColor=clr;
    update();
    }
  }


// Change bar color
void FXText::setBarColor(FXColor clr){
  if(clr!=barColor){
    barColor=clr;
    update();
    }
  }


// Set cursor color
void FXText::setCursorColor(FXColor clr){
  if(clr!=cursorColor){
    cursorColor=clr;
    update();
    }
  }


// Change text style
void FXText::setTextStyle(FXuint style){
  FXuint opts=((style^options)&TEXT_MASK)^options;
  if(options!=opts){
    options=opts;
    recalc();
    update();
    }
  }


// Get text style
FXuint FXText::getTextStyle() const {
  return (options&TEXT_MASK);
  }


// Return true if editable
FXbool FXText::isEditable() const {
  return (options&TEXT_READONLY)==0;
  }


// Set widget is editable or not
void FXText::setEditable(FXbool edit){
  options^=((edit-1)^options)&TEXT_READONLY;
  }


// Return true if text is in overstrike mode
FXbool FXText::isOverstrike() const {
  return (options&TEXT_OVERSTRIKE)!=0;
  }


// Set overstrike mode
void FXText::setOverstrike(FXbool over){
  options^=((0-over)^options)&TEXT_OVERSTRIKE;
  }


// Set styled text mode
FXbool FXText::setStyled(FXbool styled){
  if(styled && !sbuffer){
    if(!callocElms(sbuffer,length+gapend-gapstart)) return false;
    update();
    }
  if(!styled && sbuffer){
    freeElms(sbuffer);
    update();
    }
  return true;
  }


// Set highlight styles
void FXText::setHiliteStyles(FXHiliteStyle* styles){
  hilitestyles=styles;
  update();
  }


// Change number of visible rows
void FXText::setVisibleRows(FXint rows){
  if(rows<0) rows=0;
  if(vrows!=rows){
    vrows=rows;
    recalc();
    }
  }


// Change number of visible columns
void FXText::setVisibleColumns(FXint cols){
  if(cols<0) cols=0;
  if(vcols!=cols){
    vcols=cols;
    recalc();
    }
  }


// Save object to stream
void FXText::save(FXStream& store) const {
  FXScrollArea::save(store);
  store << length;
  store.save(buffer,gapstart);
  store.save(buffer+gapend,length-gapstart);
  store << nvisrows;
  store.save(visrows,nvisrows+1);
  store << margintop;
  store << marginbottom;
  store << marginleft;
  store << marginright;
  store << wrapcolumns;
  store << tabcolumns;
  store << barcolumns;
  store << font;
  store << textColor;
  store << selbackColor;
  store << seltextColor;
  store << hilitebackColor;
  store << hilitetextColor;
  store << activebackColor;
  store << numberColor;
  store << cursorColor;
  store << barColor;
  store << vrows;
  store << vcols;
  store << help;
  store << tip;
  store << matchtime;
  }


// Load object from stream
void FXText::load(FXStream& store){
  FXScrollArea::load(store);
  store >> length;
  allocElms(buffer,length+MINSIZE);
  store.load(buffer,length);
  gapstart=length;
  gapend=length+MINSIZE;
  store >> nvisrows;
  allocElms(visrows,nvisrows+1);
  store.load(visrows,nvisrows+1);
  store >> margintop;
  store >> marginbottom;
  store >> marginleft;
  store >> marginright;
  store >> wrapcolumns;
  store >> tabcolumns;
  store >> barcolumns;
  store >> font;
  store >> textColor;
  store >> selbackColor;
  store >> seltextColor;
  store >> hilitebackColor;
  store >> hilitetextColor;
  store >> activebackColor;
  store >> numberColor;
  store >> cursorColor;
  store >> barColor;
  store >> vrows;
  store >> vcols;
  store >> help;
  store >> tip;
  store >> matchtime;
  }


// Clean up
FXText::~FXText(){
  getApp()->removeTimeout(this,ID_BLINK);
  getApp()->removeTimeout(this,ID_FLASH);
  getApp()->removeTimeout(this,ID_TIPTIMER);
  freeElms(buffer);
  freeElms(sbuffer);
  freeElms(visrows);
  buffer=(FXchar*)-1L;
  sbuffer=(FXchar*)-1L;
  visrows=(FXint*)-1L;
  font=(FXFont*)-1L;
  delimiters=(const FXchar*)-1L;
  hilitestyles=(FXHiliteStyle*)-1L;
  }

}
