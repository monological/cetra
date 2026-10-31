add_cetra_app(canvas)
# Its fonts are built into the program (spec 14.6), so it reads none from beside it: the label
# face, the pixel face, and the code editor's.
cetra_embed_files(canvas fonts
    LABEL ../splash/assets/Roboto-Bold.ttf
    PIXEL ../splash/assets/PressStart2P.ttf
    EDITOR ../../assets/fonts/JetBrainsMono-Regular.ttf)
