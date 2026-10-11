add_test(NAME imports COMMAND naval-cli --schema)
set_tests_properties(imports PROPERTIES PASS_REGULAR_EXPRESSION "ObserverSnapshot navalcommand.gateway.v1.Gateway")
