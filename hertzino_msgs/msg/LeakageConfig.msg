# Leak rate and cost estimation (GET /leakage_estimation, leakage mode only).
int32 distance                # leakage_distance: 0 = 0.5 m, 1 = 1 m, 2 = 2 m, 3 = 5 m
int32 unit                    # leakage_unit: 0 l/min, 1 ml/s
string unit_name
int32 monetary_unit           # 0 CNY, 1 GBP, 2 USD, 3 EUR, 4 RUB
string monetary_unit_name
float32 value_of_gas          # currency per m3
float32 energy_cost           # currency per kWh
float32 specific_power        # kW/(m3/min)
int32 working_hours           # h per year
bool cost_display             # show the cost estimate on the camera's web page
