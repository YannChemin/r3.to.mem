MODULE_TOPDIR = $(HOME)/dev/grass

PGM = r3.to.mem

LIBES        = $(RASTER3DLIB) $(RASTERLIB) $(GISLIB)
DEPENDENCIES = $(RASTER3DDEP) $(RASTERDEP)  $(GISDEP)

include $(MODULE_TOPDIR)/include/Make/Module.make

default: cmd
