    /*
     *	make sure you are editing
     *		CONFIG.c
     *	editing config.c won't work
     */

static	char *sccsid = "@(#)CONFIG.c	2.2 (Berkeley) 82/04/01";

    /*
     *	the version of translator
     */
char	*version = "VERSION (DATE)";

    /*
     *	the location of the error strings
     *	and the length of the path to it
     *	(in case of execution as a.something)
     */
char	*err_file = "LIBDIR/ERRORSTRINGS";
int	err_pathlen = sizeof("LIBDIR/");

    /*
     *	the location of the short explanation
     *	and the length of the path to it
     *	the null at the end is so pix can change it to pi'x' from pi.
     */
char	*how_file = "LIBDIR/HOWFILE\0";
int	how_pathlen = sizeof("LIBDIR/");
    
    /*
     *	things about the interpreter.
     *	these are not used by the compiler.
     */
char	*px_header = "LIBDIR/px_header";	/* px_header's name */
char	*pi_comp = "INSTALLDIR/pi";		/* the compiler's name */
char	*px_intrp = "INSTALLDIR/px";		/* the interpreter's name */
char	*px_debug = "INSTALLDIR/pdx";		/* the debugger's name */
