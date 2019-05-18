
FOX-LITE
========

This is a lite version of *FOX GUI Library* created by Jeroen van der Zijp
(<jeroen@fox-toolkit.com>). This project includes just the essential files to
build a static library based on the original one.


## The FOX GUI Library

### 1. What Is FOX?

FOX is a C++ based Toolkit for developing Graphical User Interfaces easily and
effectively.   It offers a wide, and growing, collection of Controls, and provides
state of the art facilities such as drag and drop, selection, as well as OpenGL widgets
for 3D graphical manipulation.  FOX also implements icons, images, and user-convenience
features such as status line help, and tooltips.  Tooltips may even be used for 3D
objects!

Considerable importance has been placed on making FOX one of the fastest toolkits
around, and to minimize memory use:- FOX uses a number of techniques to speed up drawing
and spatial layout of the GUI.  Memory is conserved by allowing programmers to create
and destroy GUI elements on the fly.

Even though FOX offers a large collection of Controls already, FOX leverages C++ to
allow programmers to easily build additional Controls and GUI elements, simply by taking
existing controls, and creating a derived class which simply adds or redefines the
desired behavior.

One of the prime design goals of FOX is the ease of programming; thus, most controls
can be created using a single line of C++ code; most parameters have sensible default
values, so that they may be omitted, and layout managers ensure that designers of GUI's
do not have to worry about precise alignments.

Another nice feature of FOX which significantly reduces the number of lines of code
which have to be written is FOX's ability to have widgets connect to each other, and
passing certain commands between them; for example, a menu entry Hide Toolbar can be
directly connected to the Toolbar, and cause it to hide.

Finally, FOX makes it easy to maintain the state of the GUI in an application by having
the GUI elements automatically updating themselves by interrogating the application's
state.  This feature eliminates the large amount of effort that may go into sensitizing,
graying out, checking/unchecking etc. depending on the application state.


### 2. Where to get it?

You can FTP the complete FOX GUI toolkit from our FTP site:

	ftp://ftp.fox-toolkit.org/pub/


The distribution includes this on-line (HTML) documentation you see here.


### 3. License

The FOX Library proper is licensed under GNU Lesser GPL; all the examples and demo
programs are licensed under GPL.

If you write software based on the FOX library, please include the following in your
About Box, or some other place where a user may be able to see it:

    This software uses the FOX Toolkit (http://www.fox-toolkit.org).

Thanks!

