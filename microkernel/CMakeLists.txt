cmake_minimum_required(VERSION 3.16)

project(microkernel)

include_directories("include/")

add_executable(microkernel
	main.cpp
	src/Creators/TicketSystemCreator.cpp
	src/Creators/AddrSystemCreator.cpp
	src/Creators/TicketCreator.cpp
	src/Creators/UiCreator.cpp
	src/Systems/TicketSystem.cpp
	src/Systems/AddressSystem.cpp
	src/Models/Ticket.cpp
	src/Models/Call.cpp
	src/Controllers/Controller.cpp
	src/Controllers/CallController.cpp
	src/Controllers/UiController.cpp
	src/Ui/Ui.cpp
)
target_link_libraries(microkernel PUBLIC dl)
target_precompile_headers(microkernel PRIVATE include/json.hpp)

add_library(OpenProject SHARED ./src/lib/OpenProject.cpp src/Systems/TicketSystem.cpp)
target_link_libraries(OpenProject PUBLIC curlpp)
target_precompile_headers(OpenProject PRIVATE include/json.hpp)

add_library(DaviCal SHARED ./src/lib/DaviCal.cpp src/Systems/AddressSystem.cpp)
target_link_libraries(DaviCal PUBLIC curlpp)
target_link_libraries(DaviCal PUBLIC tinyxml)
target_precompile_headers(DaviCal PRIVATE include/json.hpp)

add_library(WebInterface SHARED ./src/lib/WebInterface.cpp src/Ui/Ui.cpp)
target_precompile_headers(WebInterface PRIVATE include/json.hpp)





#pkg_check_modules(CGICC REQUIRED cgicc)

#target_include_directories(main PUBLIC "${CGICC_INCLUDE_DIRS}")
#target_link_libraries(microkernel "${CGICC_LIBRARIES}")

#add_library(DaviCal SHARED ./src/lib/DaviCal.cpp)
#target_link_libraries(DaviCal PUBLIC curlpp)
#add_executable(OpenProjectTest src/lib/OpenProjectTest.cpp)
#target_link_libraries(OpenProjectTest PUBLIC OpenProject)

install(TARGETS microkernel RUNTIME DESTINATION bin)

