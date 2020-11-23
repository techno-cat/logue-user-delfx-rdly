# #############################################################################
# Project Customization
# #############################################################################

PROJECT = delfx_rev_delay

UCSRC = $(wildcard ../user/lib/*.c)

UCXXSRC = ../user/delay.cpp

# NOTE: Relative path from `Makefile` that refer this file.
UINCDIR = ../user/lib

UDEFS =

ULIB = 

ULIBDIR =
