cmake_minimum_required(VERSION 3.16)

# La libreria OtamaticClient vive tre livelli sopra questa cartella:
# viene aggiunta come componente esterno (il nome del componente è il nome
# della cartella, quindi "OtamaticClient").
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(advanced_example)