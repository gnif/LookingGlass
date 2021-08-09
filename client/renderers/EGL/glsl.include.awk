BEGIN { FS="\"" }

function process(line, second) {
    if (line ~ /^#include[ \t]*".+"[ \t\r]*$/) {
        while (getline < second) {
            process($0, $2)
        }
    } else {
        print line
    }
}

{ process($0, $2) }
