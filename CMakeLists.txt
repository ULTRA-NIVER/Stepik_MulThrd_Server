cmake_minimum_required(VERSION 2.8)	 # Проверка версии CMake.
										# Если версия установленой программы
										# старее указаной, произайдёт аварийный выход.

project(final)			# Название проекта

set(SOURCE_EXE epoll.cpp)		# Установка переменной со списком исходников для исполняемого файла

add_executable(final ${SOURCE_EXE})	# Создает исполняемый файл с именем main

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -pthread")

target_link_libraries(${PROJECT_NAME} PRIVATE pthread)

