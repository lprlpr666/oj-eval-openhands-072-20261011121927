#include <iostream>
#include <vector>
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>
#include <map>
#include <cassert>

using namespace std;

// RISC-V Simulator with 5-stage pipeline
// Output: Program execution results as specified in comments

class RISCVSimulator {
private:
    // Register file (32 general purpose registers)
    vector<uint32_t> registers;
    
    // Memory (simplified - using vector)
    vector<uint8_t> memory;
    
    // Program counter
    uint32_t pc;
    
    // Pipeline registers
    struct IF_ID {
        uint32_t instruction;
        uint32_t pc_plus_4;
        bool valid;
    } if_id;
    
    struct ID_EX {
        uint32_t rs1_val;
        uint32_t rs2_val;
        uint32_t imm;
        uint32_t rd;
        uint32_t pc_plus_4;
        bool reg_write;
        bool mem_read;
        bool mem_write;
        bool mem_to_reg;
        bool alu_src;
        uint32_t alu_op;
        bool valid;
    } id_ex;
    
    struct EX_MEM {
        uint32_t alu_result;
        uint32_t rs2_val;
        uint32_t rd;
        bool reg_write;
        bool mem_read;
        bool mem_write;
        bool mem_to_reg;
        bool zero;
        bool valid;
    } ex_mem;
    
    struct MEM_WB {
        uint32_t mem_data;
        uint32_t alu_result;
        uint32_t rd;
        bool reg_write;
        bool mem_to_reg;
        bool valid;
    } mem_wb;
    
    // Statistics
    uint64_t cycle_count;
    uint64_t instruction_count;
    
    // Instruction memory (simplified)
    vector<uint32_t> instructions;
    
public:
    RISCVSimulator() : registers(32, 0), memory(1024 * 1024, 0), pc(0), cycle_count(0), instruction_count(0) {
        // Initialize pipeline registers
        if_id.valid = false;
        id_ex.valid = false;
        ex_mem.valid = false;
        mem_wb.valid = false;
        
        // Set stack pointer
        registers[2] = 0x7FFFFFFF; // sp register
    }
    
    // Load program from input data
    void loadProgram() {
        string line;
        uint32_t address = 0;
        
        while (getline(cin, line)) {
            // Parse input line - assume hex format
            if (line.empty() || line[0] == '#') continue;
            
            istringstream iss(line);
            uint32_t value;
            if (iss >> hex >> value) {
                // Store in memory
                if (address < memory.size()) {
                    memory[address] = value & 0xFF;
                    memory[address + 1] = (value >> 8) & 0xFF;
                    memory[address + 2] = (value >> 16) & 0xFF;
                    memory[address + 3] = (value >> 24) & 0xFF;
                    address += 4;
                    
                    // Also store as instruction
                    instructions.push_back(value);
                }
            }
        }
        
        pc = 0; // Start from address 0
    }
    
    // Instruction decoding
    struct Instruction {
        uint32_t opcode;
        uint32_t rd;
        uint32_t rs1;
        uint32_t rs2;
        uint32_t funct3;
        uint32_t funct7;
        uint32_t imm;
        
        Instruction(uint32_t instr) {
            opcode = instr & 0x7F;
            rd = (instr >> 7) & 0x1F;
            funct3 = (instr >> 12) & 0x7;
            rs1 = (instr >> 15) & 0x1F;
            rs2 = (instr >> 20) & 0x1F;
            funct7 = (instr >> 25) & 0x7F;
            
            // Immediate extraction based on instruction type
            if (opcode == 0x37 || opcode == 0x17) { // U-type
                imm = instr & 0xFFFFF000;
            } else if (opcode == 0x63) { // B-type
                imm = ((instr >> 31) & 0x1) << 12;
                imm |= ((instr >> 7) & 0x1) << 11;
                imm |= ((instr >> 25) & 0x3F) << 5;
                imm |= ((instr >> 8) & 0xF) << 1;
                if (imm & (1 << 12)) imm |= 0xFFFFE000; // Sign extend
            } else if (opcode == 0x03) { // I-type (load)
                imm = (instr >> 20) & 0xFFF;
                if (imm & (1 << 11)) imm |= 0xFFFFF000; // Sign extend
            } else if (opcode == 0x13 || opcode == 0x67 || opcode == 0x23) { // I-type (arith/jalr/store)
                imm = (instr >> 20) & 0xFFF;
                if (imm & (1 << 11)) imm |= 0xFFFFF000; // Sign extend
            } else if (opcode == 0x6F) { // J-type
                imm = ((instr >> 31) & 0x1) << 20;
                imm |= ((instr >> 12) & 0xFF) << 12;
                imm |= ((instr >> 20) & 0x1) << 11;
                imm |= ((instr >> 21) & 0x3FF) << 1;
                if (imm & (1 << 20)) imm |= 0xFFE00000; // Sign extend
            } else {
                imm = 0;
            }
        }
    };
    
    // ALU operations
    uint32_t aluOperation(uint32_t alu_op, uint32_t rs1_val, uint32_t rs2_val, uint32_t funct3, uint32_t funct7) {
        switch (alu_op) {
            case 0: // ADD
                return rs1_val + rs2_val;
            case 1: // SUB
                return rs1_val - rs2_val;
            case 2: // AND
                return rs1_val & rs2_val;
            case 3: // OR
                return rs1_val | rs2_val;
            case 4: // XOR
                return rs1_val ^ rs2_val;
            case 5: // SLL
                return rs1_val << (rs2_val & 0x1F);
            case 6: // SRL
                return rs1_val >> (rs2_val & 0x1F);
            case 7: // SRA
                return (int32_t)rs1_val >> (rs2_val & 0x1F);
            case 8: // SLT
                return (int32_t)rs1_val < (int32_t)rs2_val ? 1 : 0;
            case 9: // SLTU
                return rs1_val < rs2_val ? 1 : 0;
            default:
                return rs1_val + rs2_val;
        }
    }
    
    // Pipeline stages
    void instructionFetch() {
        if (pc < instructions.size() * 4) {
            uint32_t instr = instructions[pc / 4];
            if_id.instruction = instr;
            if_id.pc_plus_4 = pc + 4;
            if_id.valid = true;
        } else {
            if_id.valid = false;
        }
    }
    
    void instructionDecode() {
        if (!if_id.valid) {
            id_ex.valid = false;
            return;
        }
        
        Instruction instr(if_id.instruction);
        
        // Read register values
        id_ex.rs1_val = registers[instr.rs1];
        id_ex.rs2_val = registers[instr.rs2];
        id_ex.imm = instr.imm;
        id_ex.rd = instr.rd;
        id_ex.pc_plus_4 = if_id.pc_plus_4;
        
        // Control signals (simplified)
        id_ex.reg_write = true;
        id_ex.mem_read = (instr.opcode == 0x03); // Load
        id_ex.mem_write = (instr.opcode == 0x23); // Store
        id_ex.mem_to_reg = (instr.opcode == 0x03); // Load
        id_ex.alu_src = (instr.opcode == 0x03 || instr.opcode == 0x23); // Load/Store use immediate
        
        // ALU operation determination
        if (instr.opcode == 0x33) { // R-type
            if (instr.funct7 == 0x20) {
                id_ex.alu_op = 1; // SUB
            } else {
                switch (instr.funct3) {
                    case 0: id_ex.alu_op = 0; break; // ADD
                    case 1: id_ex.alu_op = 5; break; // SLL
                    case 2: id_ex.alu_op = 8; break; // SLT
                    case 3: id_ex.alu_op = 9; break; // SLTU
                    case 4: id_ex.alu_op = 4; break; // XOR
                    case 5: id_ex.alu_op = (instr.funct7 == 0x20) ? 7 : 6; break; // SRA/SRL
                    case 6: id_ex.alu_op = 2; break; // OR
                    case 7: id_ex.alu_op = 3; break; // AND
                }
            }
        } else if (instr.opcode == 0x13) { // I-type arithmetic
            switch (instr.funct3) {
                case 0: id_ex.alu_op = 0; break; // ADDI
                case 1: id_ex.alu_op = 5; break; // SLLI
                case 2: id_ex.alu_op = 8; break; // SLTI
                case 3: id_ex.alu_op = 9; break; // SLTIU
                case 4: id_ex.alu_op = 4; break; // XORI
                case 5: id_ex.alu_op = 6; break; // SRLI/SRAI
                case 6: id_ex.alu_op = 2; break; // ORI
                case 7: id_ex.alu_op = 3; break; // ANDI
            }
        } else if (instr.opcode == 0x03 || instr.opcode == 0x23) { // Load/Store
            id_ex.alu_op = 0; // ADD for address calculation
        } else {
            id_ex.alu_op = 0;
        }
        
        id_ex.valid = true;
    }
    
    void execute() {
        if (!id_ex.valid) {
            ex_mem.valid = false;
            return;
        }
        
        uint32_t alu_input2 = id_ex.alu_src ? id_ex.imm : id_ex.rs2_val;
        ex_mem.alu_result = aluOperation(id_ex.alu_op, id_ex.rs1_val, alu_input2, 0, 0);
        ex_mem.rs2_val = id_ex.rs2_val;
        ex_mem.rd = id_ex.rd;
        ex_mem.reg_write = id_ex.reg_write;
        ex_mem.mem_read = id_ex.mem_read;
        ex_mem.mem_write = id_ex.mem_write;
        ex_mem.mem_to_reg = id_ex.mem_to_reg;
        ex_mem.zero = (ex_mem.alu_result == 0);
        ex_mem.valid = true;
    }
    
    void memoryAccess() {
        if (!ex_mem.valid) {
            mem_wb.valid = false;
            return;
        }
        
        if (ex_mem.mem_read) {
            // Load from memory
            uint32_t addr = ex_mem.alu_result;
            if (addr < memory.size() - 3) {
                mem_wb.mem_data = memory[addr] | 
                                (memory[addr + 1] << 8) |
                                (memory[addr + 2] << 16) |
                                (memory[addr + 3] << 24);
            } else {
                mem_wb.mem_data = 0;
            }
        }
        
        if (ex_mem.mem_write) {
            // Store to memory
            uint32_t addr = ex_mem.alu_result;
            if (addr < memory.size() - 3) {
                memory[addr] = ex_mem.rs2_val & 0xFF;
                memory[addr + 1] = (ex_mem.rs2_val >> 8) & 0xFF;
                memory[addr + 2] = (ex_mem.rs2_val >> 16) & 0xFF;
                memory[addr + 3] = (ex_mem.rs2_val >> 24) & 0xFF;
            }
        }
        
        mem_wb.mem_data = ex_mem.mem_read ? mem_wb.mem_data : 0;
        mem_wb.alu_result = ex_mem.alu_result;
        mem_wb.rd = ex_mem.rd;
        mem_wb.reg_write = ex_mem.reg_write;
        mem_wb.mem_to_reg = ex_mem.mem_to_reg;
        mem_wb.valid = true;
    }
    
    void writeBack() {
        if (!mem_wb.valid) return;
        
        if (mem_wb.reg_write && mem_wb.rd != 0) {
            uint32_t write_data = mem_wb.mem_to_reg ? mem_wb.mem_data : mem_wb.alu_result;
            registers[mem_wb.rd] = write_data;
        }
    }
    
    // Handle control flow (branches and jumps)
    void handleControlFlow() {
        if (!id_ex.valid) return;
        
        Instruction instr(if_id.instruction);
        
        if (instr.opcode == 0x63) { // Branch
            bool taken = false;
            switch (instr.funct3) {
                case 0: taken = (id_ex.rs1_val == id_ex.rs2_val); break; // BEQ
                case 1: taken = (id_ex.rs1_val != id_ex.rs2_val); break; // BNE
                case 4: taken = ((int32_t)id_ex.rs1_val < (int32_t)id_ex.rs2_val); break; // BLT
                case 5: taken = ((int32_t)id_ex.rs1_val >= (int32_t)id_ex.rs2_val); break; // BGE
                case 6: taken = (id_ex.rs1_val < id_ex.rs2_val); break; // BLTU
                case 7: taken = (id_ex.rs1_val >= id_ex.rs2_val); break; // BGEU
            }
            
            if (taken) {
                pc = id_ex.pc_plus_4 + instr.imm;
                // Flush pipeline
                if_id.valid = false;
            }
        } else if (instr.opcode == 0x6F) { // JAL
            registers[instr.rd] = id_ex.pc_plus_4;
            pc = id_ex.pc_plus_4 + instr.imm;
            // Flush pipeline
            if_id.valid = false;
        } else if (instr.opcode == 0x67) { // JALR
            uint32_t target = (id_ex.rs1_val + instr.imm) & ~1;
            registers[instr.rd] = id_ex.pc_plus_4;
            pc = target;
            // Flush pipeline
            if_id.valid = false;
        } else {
            pc = id_ex.pc_plus_4;
        }
    }
    
    // Run simulation
    void run() {
        // Simple simulation - run for a fixed number of cycles
        for (int cycle = 0; cycle < 1000; cycle++) {
            cycle_count++;
            
            // Pipeline stages (in reverse order to avoid hazards)
            writeBack();
            memoryAccess();
            execute();
            instructionDecode();
            instructionFetch();
            
            // Handle control flow
            handleControlFlow();
            
            // Check if we've finished
            if (!if_id.valid && !id_ex.valid && !ex_mem.valid && !mem_wb.valid) {
                break;
            }
        }
    }
    
    // Output results
    void outputResults() {
        // Output as specified in problem - return values as comments
        cout << "// RISC-V Simulation Results" << endl;
        cout << "// Cycles: " << cycle_count << endl;
        cout << "// Instructions: " << instruction_count << endl;
        cout << "// Final register state:" << endl;
        
        for (int i = 0; i < 32; i++) {
            if (registers[i] != 0) { // Only show non-zero registers
                cout << "// x" << i << " = 0x" << hex << uppercase << registers[i] << dec << endl;
            }
        }
        
        // Return value (example: value in x10/a0)
        cout << "// Return value: " << registers[10] << endl;
    }
};

int main() {
    RISCVSimulator simulator;
    
    // Load program from standard input
    simulator.loadProgram();
    
    // Run simulation
    simulator.run();
    
    // Output results
    simulator.outputResults();
    
    return 0;
}