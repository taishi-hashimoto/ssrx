#ifndef SSRX_CURSES_COMPAT_HPP
#define SSRX_CURSES_COMPAT_HPP

#define NCURSES_NOMACROS

#if defined(__has_include)
#  if __has_include(<ncurses.h>)
#    include <ncurses.h>
#    define SSRX_HAS_CURSES 1
#  elif __has_include(<curses.h>)
#    include <curses.h>
#    define SSRX_HAS_CURSES 1
#  else
#    define SSRX_HAS_CURSES 0
#  endif
#else
#  include <ncurses.h>
#  define SSRX_HAS_CURSES 1
#endif

#endif
