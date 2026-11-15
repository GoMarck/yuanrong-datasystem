if (NOT DS_TOKEN_TARGET)
    set(DS_TOKEN_TARGET token)
endif()
set(TOKEN_SRC
    client_access_token.cpp)

add_library(${DS_TOKEN_TARGET} STATIC ${TOKEN_SRC})

set(TOKEN_DEPEND_LIBS
        common_util
        ${SECUREC_LIBRARY}
        )

target_link_libraries(${DS_TOKEN_TARGET} PRIVATE ${TOKEN_DEPEND_LIBS})
