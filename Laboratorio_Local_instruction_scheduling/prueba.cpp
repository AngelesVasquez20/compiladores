// sched.cpp - Forward list scheduler para bloques ILOC
// Uso: ./sched <unidades> [-r] < bloque.iloc
//   -r : renombrado de registros (extra A)

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

// ---------------------------------------------------------------
// Estructuras
// ---------------------------------------------------------------
struct Edge {
    int other;   // el otro extremo de la arista
    int delay;   // retardo mínimo entre el inicio de i y el inicio de j
};

struct Op {
    string opcode;
    vector<string> left, right;   // operandos a la izquierda y derecha de "=>"
    int latency = 0;

    vector<string> reads, writes; // registros leídos y escritos
    bool isLoad = false;
    bool isStore = false;
    string base, offset;          // solo para loadAI / storeAI

    vector<Edge> preds, succs;    // grafo de dependencias
    int priority = 0;
    int start = 0;                // ciclo asignado (0 = aún sin programar)
};

// ---------------------------------------------------------------
// 1. Parser
// ---------------------------------------------------------------
bool isRegister(const string& s) {
    return s.size() > 1 && s[0] == 'r' && isdigit(s[1]);
}

vector<string> splitTokens(string s) {
    for (char& c : s)
        if (c == ',') c = ' ';
    istringstream in(s);
    vector<string> tokens;
    string t;
    while (in >> t) tokens.push_back(t);
    return tokens;
}

int latencyOf(const string& opcode) {
    if (opcode == "loadI") return 1;
    if (opcode == "loadAI" || opcode == "storeAI") return 3;
    if (opcode == "mult") return 2;
    if (opcode == "add" || opcode == "sub" ||
        opcode == "lshift" || opcode == "rshift") return 1;
    cerr << "Operacion desconocida: " << opcode << "\n";
    exit(1);
}

// Calcula reads, writes, base y offset a partir de left/right
void analyze(Op& op) {
    op.reads.clear();
    op.writes.clear();
    op.isLoad = (op.opcode == "loadAI");
    op.isStore = (op.opcode == "storeAI");

    if (op.isLoad) {                       // loadAI base,off => dest
        op.base = op.left[0];
        op.offset = op.left[1];
        op.reads = {op.base};
        op.writes = {op.right[0]};
    } else if (op.isStore) {               // storeAI src => base,off
        op.base = op.right[0];
        op.offset = op.right[1];
        op.reads = {op.left[0], op.base};
    } else if (op.opcode == "loadI") {     // loadI const => dest
        op.writes = {op.right[0]};
    } else {                               // add a,b => dest
        op.reads = {op.left[0], op.left[1]};
        op.writes = {op.right[0]};
    }
}

string toText(const Op& op) {
    string text = op.opcode + " ";
    for (size_t i = 0; i < op.left.size(); i++)
        text += (i ? "," : "") + op.left[i];
    text += " => ";
    for (size_t i = 0; i < op.right.size(); i++)
        text += (i ? "," : "") + op.right[i];
    return text;
}

vector<Op> readBlock() {
    vector<Op> ops;
    string line;
    while (getline(cin, line)) {
        size_t comment = line.find("//");
        if (comment != string::npos) line = line.substr(0, comment);

        size_t arrow = line.find("=>");
        if (arrow == string::npos) continue;          // línea vacía

        vector<string> left = splitTokens(line.substr(0, arrow));
        Op op;
        op.opcode = left[0];
        op.left.assign(left.begin() + 1, left.end());
        op.right = splitTokens(line.substr(arrow + 2));
        op.latency = latencyOf(op.opcode);
        analyze(op);
        ops.push_back(op);
    }
    return ops;
}

// ---------------------------------------------------------------
// Extra A: renombrado de registros
// ---------------------------------------------------------------
void renameRegisters(vector<Op>& ops) {
    // el primer nombre nuevo es mayor que cualquier registro usado
    int next = 0;
    for (const Op& op : ops)
        for (const auto* list : {&op.left, &op.right})
            for (const string& t : *list)
                if (isRegister(t)) next = max(next, stoi(t.substr(1)));
    next++;

    map<string, string> current;   // nombre original -> nombre actual

    for (Op& op : ops) {
        // 1) renombrar lecturas
        for (string& t : op.left)
            if (isRegister(t) && current.count(t)) t = current[t];
        if (op.isStore)                       // base de storeAI es lectura
            if (isRegister(op.right[0]) && current.count(op.right[0]))
                op.right[0] = current[op.right[0]];

        // 2) renombrar la escritura (r0 no se renombra)
        if (!op.isStore && op.right[0] != "r0") {
            string fresh = "r" + to_string(next++);
            current[op.right[0]] = fresh;
            op.right[0] = fresh;
        }
        analyze(op);
    }
}

// ---------------------------------------------------------------
// 2. Grafo de dependencias
// ---------------------------------------------------------------
void addEdge(vector<Op>& ops, int from, int to, int delay) {
    for (Edge& e : ops[from].succs) {
        if (e.other == to) {                    // ya existe: dejar el mayor
            e.delay = max(e.delay, delay);
            for (Edge& p : ops[to].preds)
                if (p.other == from) p.delay = max(p.delay, delay);
            return;
        }
    }
    ops[from].succs.push_back({to, delay});
    ops[to].preds.push_back({from, delay});
}

bool memoryConflict(const Op& a, const Op& b) {
    if (!(a.isLoad || a.isStore) || !(b.isLoad || b.isStore)) return false;
    if (a.isLoad && b.isLoad) return false;     // load con load, nunca
    if (a.base != b.base) return true;          // bases distintas: suponer conflicto
    return a.offset == b.offset;                // misma base: depende del desplazamiento
}

void buildGraph(vector<Op>& ops) {
    map<string, int> lastWriter;                // registro -> última op que lo escribió
    map<string, vector<int>> readersSince;      // registro -> lectores desde esa escritura

    for (int i = 0; i < (int)ops.size(); i++) {
        // Dependencias verdaderas: leo algo que alguien escribió
        for (const string& reg : ops[i].reads)
            if (lastWriter.count(reg)) {
                int w = lastWriter[reg];
                addEdge(ops, w, i, ops[w].latency);
            }

        // Dependencias anti y de salida: escribo algo
        for (const string& reg : ops[i].writes) {
            for (int r : readersSince[reg])                  // anti
                if (r != i) addEdge(ops, r, i, 1);
            if (lastWriter.count(reg)) {                     // salida
                int w = lastWriter[reg];
                addEdge(ops, w, i, ops[w].latency);
            }
        }

        // Actualizar el estado
        for (const string& reg : ops[i].reads) readersSince[reg].push_back(i);
        for (const string& reg : ops[i].writes) {
            readersSince[reg].clear();
            lastWriter[reg] = i;
        }
    }

    // Dependencias de memoria
    for (int i = 0; i < (int)ops.size(); i++)
        for (int j = 0; j < i; j++) {
            if (!memoryConflict(ops[j], ops[i])) continue;
            if (ops[j].isStore) addEdge(ops, j, i, ops[j].latency);  // store -> load/store
            else               addEdge(ops, j, i, 1);                // load -> store
        }
}

// ---------------------------------------------------------------
// 3. Prioridad: prio(n) = lat(n) + max prio(sucesores)
// ---------------------------------------------------------------
void computePriorities(vector<Op>& ops) {
    for (int i = (int)ops.size() - 1; i >= 0; i--) {   // las aristas siempre van hacia adelante
        int best = 0;
        for (const Edge& e : ops[i].succs)
            best = max(best, ops[e.other].priority);
        ops[i].priority = ops[i].latency + best;
    }
}

// ---------------------------------------------------------------
// 4. Forward list scheduling
// ---------------------------------------------------------------
bool isReady(const vector<Op>& ops, int i, int cycle) {
    for (const Edge& p : ops[i].preds) {
        int start = ops[p.other].start;
        if (start == 0 || start + p.delay > cycle) return false;
    }
    return true;
}

void schedule(vector<Op>& ops, int units) {
    int scheduled = 0;
    int cycle = 1;

    while (scheduled < (int)ops.size()) {
        // lista de operaciones listas en este ciclo
        vector<int> ready;
        for (int i = 0; i < (int)ops.size(); i++)
            if (ops[i].start == 0 && isReady(ops, i, cycle))
                ready.push_back(i);

        // mayor prioridad primero; stable_sort deja primero a la que aparece antes
        stable_sort(ready.begin(), ready.end(), [&](int a, int b) {
            return ops[a].priority > ops[b].priority;
        });

        // una operación por unidad disponible
        for (int k = 0; k < (int)ready.size() && k < units; k++) {
            ops[ready[k]].start = cycle;
            scheduled++;
        }
        cycle++;
    }
}

// ---------------------------------------------------------------
// 5. Salida
// ---------------------------------------------------------------
void printSchedule(const vector<Op>& ops) {
    int lastIssue = 0, length = 0;
    for (const Op& op : ops) {
        lastIssue = max(lastIssue, op.start);
        length = max(length, op.start + op.latency);
    }
    length -= 1;

    for (int cycle = 1; cycle <= lastIssue; cycle++) {
        string line;
        for (const Op& op : ops)
            if (op.start == cycle)
                line += (line.empty() ? "" : " ; ") + toText(op);
        cout << cycle << ": " << (line.empty() ? "nop" : line) << "\n";
    }
    cout << "length: " << length << " cycles\n";
}

// ---------------------------------------------------------------
int main(int argc, char* argv[]) {
    int units = 1;
    bool rename = false;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-r") rename = true;
        else units = stoi(arg);
    }

    vector<Op> ops = readBlock();
    if (rename) renameRegisters(ops);
    buildGraph(ops);
    computePriorities(ops);
    schedule(ops, units);
    printSchedule(ops);
    return 0;
}
