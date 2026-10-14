#utils.sh

function say_hello(){
    echo "hello"
    echo "first: $1, second: $2"
    echo "count: $#"
    echo "all: $@"
}