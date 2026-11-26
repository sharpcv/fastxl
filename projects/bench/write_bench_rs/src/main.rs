//! Writes a deterministic synthetic workbook with `rust_xlsxwriter`.
//!
//! Part of the fastxl write benchmark: `projects/bench/compare_write.py` starts
//! this binary (release build) and compares wall time, throughput and peak RSS
//! against fastxl, openpyxl and the Python `xlsxwriter`. The generated rows are
//! identical to the other implementations:
//!
//!   col 0: integer row number
//!   col 1: text "name-<row % 997>"
//!   col 2: float row * 0.5
//!   col 3: bool (row % 3 == 0)
//!   col 4: text "bucket-<row % 12>"
//!   col 5..cols: numeric row * column
//!
//! Usage: `write_bench_rs --rows 200000 --cols 8 --out /tmp/bench.xlsx [--json]`

use std::fs;
use std::time::Instant;

use rust_xlsxwriter::{Workbook, XlsxError};

const NAME_MOD: u32 = 997;
const BUCKET_MOD: u32 = 12;

fn peak_rss_mb() -> f64 {
    // VmHWM is the process high-water mark in kB (Linux only).
    if let Ok(status) = fs::read_to_string("/proc/self/status") {
        for line in status.lines() {
            if let Some(rest) = line.strip_prefix("VmHWM:") {
                let kb: f64 = rest
                    .trim()
                    .trim_end_matches(" kB")
                    .parse()
                    .unwrap_or(0.0);
                return kb / 1024.0;
            }
        }
    }
    0.0
}

fn main() -> Result<(), XlsxError> {
    let mut rows: u32 = 200_000;
    let mut cols: u32 = 8;
    let mut out = String::from("bench.xlsx");
    let mut json = false;
    let args: Vec<String> = std::env::args().collect();
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--rows" => {
                rows = args.get(index + 1).and_then(|v| v.parse().ok()).unwrap_or(rows);
                index += 2;
            }
            "--cols" => {
                cols = args.get(index + 1).and_then(|v| v.parse().ok()).unwrap_or(cols);
                index += 2;
            }
            "--out" => {
                out = args.get(index + 1).cloned().unwrap_or(out);
                index += 2;
            }
            "--json" => {
                json = true;
                index += 1;
            }
            other => panic!("unknown argument: {other}"),
        }
    }

    // Build the same in-memory rows the Python implementations pass to their
    // writers, so the timed section is the write itself, not data generation.
    enum Cell {
        Int(u32),
        Text(String),
        Float(f64),
        Bool(bool),
    }
    let mut table: Vec<Vec<Cell>> = Vec::with_capacity(rows as usize);
    for row in 0..rows {
        let mut cells: Vec<Cell> = Vec::with_capacity(cols as usize);
        cells.push(Cell::Int(row));
        cells.push(Cell::Text(format!("name-{}", row % NAME_MOD)));
        cells.push(Cell::Float(row as f64 * 0.5));
        cells.push(Cell::Bool(row % 3 == 0));
        cells.push(Cell::Text(format!("bucket-{}", row % BUCKET_MOD)));
        for col in 5..cols {
            cells.push(Cell::Float(row as f64 * col as f64));
        }
        table.push(cells);
    }

    let header: Vec<String> = (0..cols).map(|index| format!("c{index}")).collect();

    let mut book = Workbook::new();
    let sheet = book.add_worksheet();

    let started = Instant::now();
    for (col, name) in header.iter().enumerate() {
        let column = u16::try_from(col).expect("column index fits u16");
        sheet.write_string(0, column, name)?;
    }
    for (row, cells) in table.iter().enumerate() {
        // Data starts below the header row, matching the Python implementations.
        let row = u32::try_from(row + 1).expect("row index fits u32");
        for (col, cell) in cells.iter().enumerate() {
            let column = u16::try_from(col).expect("column index fits u16");
            match cell {
                Cell::Int(value) => {
                    sheet.write(row, column, *value)?;
                }
                Cell::Text(value) => {
                    sheet.write_string(row, column, value)?;
                }
                Cell::Float(value) => {
                    sheet.write(row, column, *value)?;
                }
                Cell::Bool(value) => {
                    sheet.write_boolean(row, column, *value)?;
                }
            }
        }
    }
    book.save(&out)?;
    let seconds = started.elapsed().as_secs_f64();

    let bytes = fs::metadata(&out).map(|meta| meta.len()).unwrap_or(0);
    let peak_mb = peak_rss_mb();
    let result = format!(
        "{{\"implementation\":\"rust_xlsxwriter\",\"rows\":{rows},\"cols\":{cols},\
         \"seconds\":{seconds:.4},\"bytes\":{bytes},\"peak_mb\":{peak_mb:.1}}}"
    );
    if json {
        println!("{result}");
    } else {
        println!(
            "rust_xlsxwriter: {rows} rows x {cols} cols in {seconds:.3}s \
             ({:.0} rows/s), {bytes} bytes",
            rows as f64 / seconds
        );
    }
    Ok(())
}
