# Topic — Emergency Department (ED)

**Emergency Department (ED)** operates 24 hours a day, providing immediate help to people in life- or health-threatening conditions. The ED’s operation is based on **triage**, i.e. assessing patients’ condition to determine priority of care (this does **not** follow arrival order). The waiting room has **N** places.

## Rules of operation (ED)

* The ED is open 24 hours a day.
* At any moment the ED waiting room may contain **at most N** patients (any additional patients must wait before entering).
* Children under **18 years** come to the ED under the care of an adult.
* Entitled VIPs (e.g., honorary blood donors) approach registration without queuing.
* Every patient must register before seeing a doctor.
* There are **2 registration windows**; at least **one** window is always operating.
* If more than **K** patients are waiting in the registration queue (where **K ≥ N/2**), the **second** registration window opens.
* The second window closes when the number of patients in the registration queue is **less than N/3**.

---

## Course of a visit to the ED

### A. Registration

* The patient provides personal details and describes symptoms.

### B. Health assessment (Triage)

* The primary care doctor (or triage doctor) verifies the patient’s condition and assigns a **color code** corresponding to urgency. This determines who receives care first:

    * **Red** — indicates immediate threat to health or life; requires immediate help — approx. **10%** of patients.
    * **Yellow** — urgent case; the patient should be admitted as soon as possible — approx. **35%** of patients.
    * **Green** — stable case, no serious health impairment or life threat; patient will be treated after red and yellow cases — approx. **50%** of patients.
* About **5%** of patients are sent home directly from triage.
* After assigning a color, the triage doctor directs the patient to an appropriate specialist (e.g., cardiologist, neurologist, ophthalmologist, ENT, surgeon, paediatrician).

### C. Initial diagnostics and treatment

* The specialist performs necessary examinations (medical history, physical exam, ECG, blood pressure measurement, etc.) to stabilise the patient’s vital functions.

### D. Decision on further management

After initial diagnosis and stabilisation, the specialist may decide to:

* **Discharge the patient home** — approx. **85%** of patients.
* **Refer the patient for further treatment** to an appropriate hospital ward — approx. **14.5%** of patients.
* **Refer the patient to another specialised facility** (e.g., due to lack of a specialist or lack of space on a hospital ward) — approx. **0.5%** of patients.

---

## Director’s orders / Special signals

* On the Director’s order (**signal 1**) a given specialist stops their ED work to examine a current patient and proceeds to the ward. That specialist returns to the ED after a randomly determined time.
* On the Director’s order (**signal 2**) all patients and doctors immediately evacuate the building.
