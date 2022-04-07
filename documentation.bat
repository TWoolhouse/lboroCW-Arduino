@echo Compiling Documentation
@pandoc -N -o output.pdf --template=coa202.latex README.md --shift-heading-level-by=-1
@echo Done!
