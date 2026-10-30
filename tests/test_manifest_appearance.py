"""Check the manifest lookup's SQL and actual C++ decision path offline."""
import argparse
import re
import sqlite3
import subprocess
import tempfile
from pathlib import Path

if not __debug__:
    raise SystemExit('Run without Python optimization')

parser = argparse.ArgumentParser()
parser.add_argument('--build-guard', required=True)
parser.add_argument('--work-dir', required=True)
args = parser.parse_args()

source = (Path(__file__).resolve().parents[1] / 'src/Bot/Factory/RandomPlayerbotFactory.cpp').read_text()
start = source.index('    QueryResult existing =', source.index('uint32 RandomPlayerbotFactory::EnsureManifestBot'))
end = source.index('    WorldSession session', start)
body = source[start:end]
query = body.split('spec.name, spec.name', 1)[0]
sql = ''.join(re.findall(r'"([^"\n]*)"', query)).replace("'{}'", '?')

with sqlite3.connect(':memory:') as db:
    db.execute('CREATE TABLE characters (guid INTEGER, account INTEGER, race INTEGER, class INTEGER, '
               'level INTEGER, gender INTEGER, skin INTEGER, face INTEGER, hairStyle INTEGER, '
               'hairColor INTEGER, facialStyle INTEGER, name TEXT)')
    assert db.execute(sql, ('test', 'test')).fetchall() == [(0,) * 12]
    db.execute("INSERT INTO characters VALUES (7,22,3,4,1,0,5,6,7,8,9,'test')")
    assert db.execute(sql, ('test', 'test')).fetchall() == [(1,7,22,3,4,1,0,5,6,7,8,9)]
    db.execute("UPDATE characters SET guid = 0 WHERE name = 'test'")
    assert db.execute(sql, ('test', 'test')).fetchall() == [(1,0,22,3,4,1,0,5,6,7,8,9)]

cpp = '''#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
using uint8 = uint8_t;
using uint32 = uint32_t;
struct Field { uint32 value; template<class T> T Get() { return T(value); } };
struct Result { Field fields[12]; Field* Fetch() { return fields; } };
using QueryResult = std::shared_ptr<Result>;
struct DB { QueryResult result; template<class... T> QueryResult Query(T...) { return result; } } CharacterDatabase;
struct Spec { std::string name; uint8 race, cls, gender, skin, face, hairStyle, hairColor, facialHair; };
uint32 test(uint32 accountId, Spec spec) {
''' + body + '''    return 42;
}
int main() {
    Spec spec{"example", 3, 4, 0, 5, 6, 7, 8, 9};
    assert(test(22, spec) == 0);
    auto result = std::make_shared<Result>();
    CharacterDatabase.result = result;
    assert(test(22, spec) == 42);
    unsigned values[] = {1, 7, 22, 3, 4, 1, 0, 5, 6, 7, 8, 9};
    for (int i = 0; i < 12; ++i) result->fields[i].value = values[i];
    assert(test(22, spec) == 7);
    result->fields[1].value = 0;
    assert(test(22, spec) == 0);
    result->fields[1].value = 7;
    for (int i = 2; i < 12; ++i) {
        ++result->fields[i].value;
        assert(test(22, spec) == 0);
        --result->fields[i].value;
    }
}
'''
with tempfile.TemporaryDirectory(dir=args.work_dir) as temp:
    fixture = Path(temp) / 'fixture.cpp'
    binary = Path(temp) / 'fixture'
    fixture.write_text(cpp)
    subprocess.run([args.build_guard], check=True)
    subprocess.run(['c++', '-std=c++17', str(fixture), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Manifest lookup: SQL, failure, absence, match and mismatches passed')
