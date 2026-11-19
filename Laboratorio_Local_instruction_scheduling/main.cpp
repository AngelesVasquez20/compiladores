// Get-Content t2.iloc | .\sched.exe 1 -r
// sched.exe 1 < t1.iloc
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
using namespace std;

struct Edge
{
    int other;
    int delay;
};

struct Op
{
    string opcode;
    vector<string> left, right;
    int latency = 0;

    vector<string> reads, writes;
    bool isLoad = false;
    bool isStore = false;
    string base, offset;

    vector<Edge> preds, succs;
    int priority = 0;
    int start = 0;
};

bool isRegister(const string &s)
{
    return s.size() > 1 && s[0] == 'r' && isdigit(s[1]);
}

vector<string> splitTokens(string s)
{
    for (char &c : s)
        if (c == ',')
            c = ' ';
    istringstream in(s);
    vector<string> tokens;
    string t;
    while (in >> t)
        tokens.push_back(t);
    return tokens;
}

int latencyOf(const string &opcode)
{
    if (opcode == "loadI") return 1;
    if (opcode == "loadAI" || opcode == "storeAI") return 3;
    if (opcode == "mult") return 2;
    if (opcode == "add" || opcode == "sub" || opcode == "lshift" || opcode == "rshift") return 1;
    cerr << "Operacion desconocida: " << opcode << "\n";
    exit(1);
}

void analyze(Op &op)
{
    op.reads.clear();
    op.writes.clear();
    op.isLoad = (op.opcode == "loadAI");
    op.isStore = (op.opcode == "storeAI");

    if (op.isLoad)
    {
        op.base = op.left[0];
        op.offset = op.left[1];
        op.reads = {op.base};
        op.writes = {op.right[0]};
    }
    else if (op.isStore)
    {
        op.base = op.right[0];
        op.offset = op.right[1];
        op.reads = {op.left[0], op.base};
    }
    else if (op.opcode == "loadI")
    {
        op.writes = {op.right[0]};
    }
    else
    {
        op.reads = {op.left[0], op.left[1]};
        op.writes = {op.right[0]};
    }
}

string toText(const Op &op)
{
    string text = op.opcode + " ";
    for (size_t i = 0; i < op.left.size(); i++)
    {
        if (i > 0)
            text += ",";
        text += op.left[i];
    }
    text += " => ";
    for (size_t i = 0; i < op.right.size(); i++)
    {
        if (i > 0)
            text += ",";
        text += op.right[i];
    }
    return text;
}

vector<Op> readBlock()
{
    vector<Op> ops;
    string line;
    while (getline(cin, line))
    {
        size_t comment = line.find("//");
        if (comment != string::npos)
            line = line.substr(0, comment);

        size_t arrow = line.find("=>");
        if (arrow == string::npos)
            continue;

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

int biggestRegister(const vector<string> &tokens)
{
    int biggest = 0;
    for (const string &t : tokens)
        if (isRegister(t))
            biggest = max(biggest, stoi(t.substr(1)));
    return biggest;
}

void renameRegisters(vector<Op> &ops)
{
    int next = 1;
    for (const Op &op : ops)
    {
        next = max(next, biggestRegister(op.left) + 1);
        next = max(next, biggestRegister(op.right) + 1);
    }

    map<string, string> current;

    for (Op &op : ops)
    {
        for (string &t : op.left)
            if (current.count(t))
                t = current[t];
        if (op.isStore && current.count(op.right[0]))
            op.right[0] = current[op.right[0]];

        if (!op.isStore && op.right[0] != "r0")
        {
            string fresh = "r" + to_string(next);
            next++;
            current[op.right[0]] = fresh;
            op.right[0] = fresh;
        }
        analyze(op);
    }
}

void addEdge(vector<Op> &ops, int from, int to, int delay)
{
    ops[from].succs.push_back({to, delay});
    ops[to].preds.push_back({from, delay});
}

bool memoryConflict(const Op &a, const Op &b)
{
    if (!(a.isLoad || a.isStore) || !(b.isLoad || b.isStore))
        return false;
    if (a.isLoad && b.isLoad)
        return false;
    if (a.base != b.base)
        return true;
    return a.offset == b.offset;
}

void buildGraph(vector<Op> &ops)
{
    map<string, int> lastWriter;
    map<string, vector<int>> readersSince;

    for (int i = 0; i < (int)ops.size(); i++)
    {
        for (const string &reg : ops[i].reads)
            if (lastWriter.count(reg))
            {
                int w = lastWriter[reg];
                addEdge(ops, w, i, ops[w].latency);
            }

        for (const string &reg : ops[i].writes)
        {
            for (int r : readersSince[reg])
                addEdge(ops, r, i, 1);
            if (lastWriter.count(reg))
            {
                int w = lastWriter[reg];
                addEdge(ops, w, i, ops[w].latency);
            }
        }

        for (const string &reg : ops[i].reads)
            readersSince[reg].push_back(i);
        for (const string &reg : ops[i].writes)
        {
            readersSince[reg].clear();
            lastWriter[reg] = i;
        }
    }

    for (int i = 0; i < (int)ops.size(); i++)
        for (int j = 0; j < i; j++)
        {
            if (!memoryConflict(ops[j], ops[i]))
                continue;
            if (ops[j].isStore)
                addEdge(ops, j, i, ops[j].latency);
            else
                addEdge(ops, j, i, 1);
        }
}

void computePriorities(vector<Op> &ops)
{
    for (int i = (int)ops.size() - 1; i >= 0; i--)
    {
        int best = 0;
        for (const Edge &e : ops[i].succs)
            best = max(best, ops[e.other].priority);
        ops[i].priority = ops[i].latency + best;
    }
}

bool isReady(const vector<Op> &ops, int i, int cycle)
{
    for (const Edge &p : ops[i].preds)
    {
        int start = ops[p.other].start;
        if (start == 0 || start + p.delay > cycle)
            return false;
    }
    return true;
}

int findBestReady(const vector<Op> &ops, int cycle)
{
    int best = -1;
    for (int i = 0; i < (int)ops.size(); i++)
    {
        if (ops[i].start != 0 || !isReady(ops, i, cycle))
            continue;
        if (best == -1 || ops[i].priority > ops[best].priority)
            best = i;
    }
    return best;
}

void schedule(vector<Op> &ops, int units)
{
    int scheduled = 0;
    int cycle = 1;

    while (scheduled < (int)ops.size())
    {
        for (int unit = 0; unit < units; unit++)
        {
            int i = findBestReady(ops, cycle);
            if (i == -1)
                break;
            ops[i].start = cycle;
            scheduled++;
        }
        cycle++;
    }
}

void printSchedule(const vector<Op> &ops)
{
    int lastIssue = 0, length = 0;
    for (const Op &op : ops)
    {
        lastIssue = max(lastIssue, op.start);
        length = max(length, op.start + op.latency);
    }
    length -= 1;

    for (int cycle = 1; cycle <= lastIssue; cycle++)
    {
        string line = "";
        for (const Op &op : ops)
        {
            if (op.start != cycle)
                continue;
            if (line != "")
                line += " ; ";
            line += toText(op);
        }
        if (line == "")
            line = "nop";
        cout << cycle << ": " << line << "\n";
    }
    cout << "length: " << length << " cycles\n";
}

int main(int argc, char *argv[])
{
    int units = 1;
    bool rename = false;
    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "-r")
            rename = true;
        else
            units = stoi(arg);
    }

    vector<Op> ops = readBlock();
    if (rename)
        renameRegisters(ops);
    buildGraph(ops);
    computePriorities(ops);
    schedule(ops, units);
    printSchedule(ops);
    return 0;
}
